// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Open forks. FSOpenFork and FSpOpenDF hand out reference numbers from one
// table, as the File Manager does. Data forks are the files themselves; a
// resource fork is read from the file's AppleDouble companion and cannot be
// written. Reads and writes use pread and pwrite at the fork's mark, so
// threads sharing the process never share a file position.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "carbon.h"

// What FSGetResourceForkName returns: "RESOURCE_FORK".
static const UniChar kResourceForkName[] = {
  'R', 'E', 'S', 'O', 'U', 'R', 'C', 'E', '_', 'F', 'O', 'R', 'K',
};
#define RESOURCE_FORK_NAME_LENGTH 13

#define FIRST_REFNUM 100
#define MAX_FORKS 1024

typedef struct {
  int in_use;
  // -1 for an empty resource fork, which reads as zero bytes.
  int fd;
  int writable;
  int resource;
  // Where the fork starts in fd, and a resource fork's fixed length.
  off_t base;
  off_t length;
  off_t mark;
  char path[PATH_MAX];
} fork_entry;

static pthread_mutex_t forks_lock = PTHREAD_MUTEX_INITIALIZER;
static fork_entry forks[MAX_FORKS];

// Called with forks_lock held.
static fork_entry* fork_for(SInt16 refnum) {
  int i = refnum - FIRST_REFNUM;
  if (i < 0 || i >= MAX_FORKS || !forks[i].in_use) {
    return NULL;
  }
  return &forks[i];
}

static off_t fork_size(const fork_entry* f) {
  if (f->resource) {
    return f->length;
  }
  struct stat st;
  return fstat(f->fd, &st) == 0 ? st.st_size : 0;
}

// The offset a positionMode and offset name. Newline and cache bits in
// the mode are ignored.
static OSErr resolve_position(const fork_entry* f, UInt16 mode, SInt64 offset,
                              off_t* out) {
  off_t base;
  switch (mode & 3) {
    case fsAtMark:
      *out = f->mark;
      return noErr;
    case fsFromStart:
      base = 0;
      break;
    case fsFromLEOF:
      base = fork_size(f);
      break;
    default:
      base = f->mark;
      break;
  }
  if (base + offset < 0) {
    return posErr;
  }
  *out = base + offset;
  return noErr;
}

static int fork_kind(UniCharCount length, const UniChar* name) {
  if (length == 0) {
    return 0;
  }
  if (length == RESOURCE_FORK_NAME_LENGTH &&
      !memcmp(name, kResourceForkName, sizeof(kResourceForkName))) {
    return 1;
  }
  return -1;
}

static OSErr open_fork(const char* path, int resource, SInt8 permission,
                       SInt16* refnum) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return fnfErr;
  }
  if (S_ISDIR(st.st_mode)) {
    return notAFileErr;
  }
  int writable = permission == fsWrPerm || permission == fsRdWrPerm ||
                 permission == fsRdWrShPerm;
  int fd = -1;
  off_t base = 0;
  off_t length = 0;
  if (resource) {
    if (writable) {
      cf_warn_once("writing a resource fork");
      return wrPermErr;
    }
    hle_appledouble ad;
    if (hle_appledouble_read(path, &ad) && ad.has_resource_fork) {
      fd = open(ad.path, O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return hle_oserr_from_errno(errno);
      }
      base = ad.resource_offset;
      length = ad.resource_length;
    }
  } else if (permission == fsCurPerm) {
    // Whatever access the file allows.
    fd = open(path, O_RDWR | O_CLOEXEC);
    writable = fd >= 0;
    if (fd < 0) {
      fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
      return hle_oserr_from_errno(errno);
    }
  } else {
    fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) {
      return errno == EACCES && writable ? wrPermErr
                                         : hle_oserr_from_errno(errno);
    }
  }

  pthread_mutex_lock(&forks_lock);
  int i;
  for (i = 0; i < MAX_FORKS && forks[i].in_use; i++) {
  }
  if (i == MAX_FORKS) {
    pthread_mutex_unlock(&forks_lock);
    if (fd >= 0) {
      close(fd);
    }
    return tmfoErr;
  }
  fork_entry* f = &forks[i];
  f->in_use = 1;
  f->fd = fd;
  f->writable = writable;
  f->resource = resource;
  f->base = base;
  f->length = length;
  f->mark = 0;
  snprintf(f->path, sizeof(f->path), "%s", path);
  *refnum = FIRST_REFNUM + i;
  pthread_mutex_unlock(&forks_lock);
  cf_trace("open %s fork of %s = %d", resource ? "resource" : "data", path,
           FIRST_REFNUM + i);
  return noErr;
}

// ---------------------------------------------------------------------------
// FSRef forks

int FSOpenFork(const FSRef* ref, UniCharCount length, const UniChar* name,
               SInt8 permission, SInt16* refnum) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  int kind = fork_kind(length, name);
  if (kind < 0) {
    return errFSForkNotFound;
  }
  return open_fork(path, kind, permission, refnum);
}

int FSReadFork(SInt16 refnum, UInt16 mode, SInt64 offset, ByteCount count,
               void* buffer, ByteCount* actual) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  off_t position = 0;
  OSErr err = f ? resolve_position(f, mode, offset, &position)
                : errFSBadForkRef;
  fork_entry snapshot;
  if (f) {
    snapshot = *f;
  }
  pthread_mutex_unlock(&forks_lock);
  if (actual) {
    *actual = 0;
  }
  if (err) {
    return err;
  }

  ssize_t done = 0;
  if (snapshot.resource) {
    off_t available = snapshot.length - position;
    size_t want = available <= 0 ? 0
                  : (off_t)count < available ? count
                                             : (size_t)available;
    if (want && snapshot.fd >= 0) {
      done = pread(snapshot.fd, buffer, want, snapshot.base + position);
    }
  } else {
    while ((ByteCount)done < count) {
      ssize_t n = pread(snapshot.fd, (char*)buffer + done, count - done,
                        position + done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        if (n < 0) {
          done = done ? done : -1;
        }
        break;
      }
      done += n;
    }
  }
  if (done < 0) {
    return ioErr;
  }

  pthread_mutex_lock(&forks_lock);
  f = fork_for(refnum);
  if (f) {
    f->mark = position + done;
  }
  pthread_mutex_unlock(&forks_lock);
  if (actual) {
    *actual = done;
  }
  return (ByteCount)done < count ? eofErr : noErr;
}

int FSWriteFork(SInt16 refnum, UInt16 mode, SInt64 offset, ByteCount count,
                const void* buffer, ByteCount* actual) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  off_t position = 0;
  OSErr err = !f ? errFSBadForkRef
              : !f->writable ? wrPermErr
                             : resolve_position(f, mode, offset, &position);
  int fd = f ? f->fd : -1;
  pthread_mutex_unlock(&forks_lock);
  if (actual) {
    *actual = 0;
  }
  if (err) {
    return err;
  }
  size_t done = 0;
  while (done < count) {
    ssize_t n = pwrite(fd, (const char*)buffer + done, count - done,
                       position + done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      err = n < 0 ? hle_oserr_from_errno(errno) : ioErr;
      break;
    }
    done += n;
  }
  pthread_mutex_lock(&forks_lock);
  f = fork_for(refnum);
  if (f) {
    f->mark = position + done;
  }
  pthread_mutex_unlock(&forks_lock);
  if (actual) {
    *actual = done;
  }
  return err;
}

int FSCloseFork(SInt16 refnum) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  if (!f) {
    pthread_mutex_unlock(&forks_lock);
    return errFSBadForkRef;
  }
  if (f->fd >= 0) {
    close(f->fd);
  }
  f->in_use = 0;
  pthread_mutex_unlock(&forks_lock);
  return noErr;
}

// Writes reach the kernel as they are made, so nothing is left to flush
// that the game ending would lose. The disk itself is not synced.
int FSFlushFork(SInt16 refnum) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  pthread_mutex_unlock(&forks_lock);
  return f ? noErr : errFSBadForkRef;
}

int FSGetForkSize(SInt16 refnum, SInt64* size) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  if (f) {
    *size = fork_size(f);
  }
  pthread_mutex_unlock(&forks_lock);
  return f ? noErr : errFSBadForkRef;
}

int FSSetForkSize(SInt16 refnum, UInt16 mode, SInt64 offset) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  off_t size = 0;
  OSErr err = !f ? errFSBadForkRef
              : !f->writable ? wrPermErr
                             : resolve_position(f, mode, offset, &size);
  if (!err && ftruncate(f->fd, size) != 0) {
    err = hle_oserr_from_errno(errno);
  }
  if (!err && f->mark > size) {
    f->mark = size;
  }
  pthread_mutex_unlock(&forks_lock);
  return err;
}

int FSGetForkPosition(SInt16 refnum, SInt64* position) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  if (f) {
    *position = f->mark;
  }
  pthread_mutex_unlock(&forks_lock);
  return f ? noErr : errFSBadForkRef;
}

// Past the end of the fork, the mark stops at the end and eofErr says so.
int FSSetForkPosition(SInt16 refnum, UInt16 mode, SInt64 offset) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  off_t position = 0;
  OSErr err = f ? resolve_position(f, mode, offset, &position)
                : errFSBadForkRef;
  if (!err) {
    off_t size = fork_size(f);
    if (position > size) {
      position = size;
      err = eofErr;
    }
    f->mark = position;
  }
  pthread_mutex_unlock(&forks_lock);
  return err;
}

int FSAllocateFork(SInt16 refnum, UInt16 flags, UInt16 mode, SInt64 offset,
                   UInt64 request, UInt64* actual) {
  pthread_mutex_lock(&forks_lock);
  fork_entry* f = fork_for(refnum);
  pthread_mutex_unlock(&forks_lock);
  if (!f) {
    return errFSBadForkRef;
  }
  if (actual) {
    *actual = request;
  }
  return noErr;
}

int FSCreateFork(const FSRef* ref, UniCharCount length, const UniChar* name) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  int kind = fork_kind(length, name);
  if (kind < 0) {
    return errFSBadForkName;
  }
  hle_appledouble ad;
  if (kind == 0 ||
      (hle_appledouble_read(path, &ad) && ad.has_resource_fork)) {
    return errFSForkExists;
  }
  return noErr;  // an empty resource fork reads as one already
}

int FSIterateForks(const FSRef* ref, CatPositionRec* position,
                   HFSUniStr255* name, SInt64* size, UInt64* physical_size) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  if (position->initialize == 0) {
    position->initialize = 1;
    position->priv[0] = 0;
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    return fnfErr;
  }
  if (position->priv[0] == 0 && !S_ISDIR(st.st_mode)) {
    position->priv[0] = 1;
    if (name) {
      name->length = 0;
    }
    if (size) {
      *size = st.st_size;
    }
    if (physical_size) {
      *physical_size = (UInt64)st.st_blocks * 512;
    }
    return noErr;
  }
  hle_appledouble ad;
  if (position->priv[0] == 1 && hle_appledouble_read(path, &ad) &&
      ad.has_resource_fork) {
    position->priv[0] = 2;
    if (name) {
      name->length = RESOURCE_FORK_NAME_LENGTH;
      memcpy(name->unicode, kResourceForkName, sizeof(kResourceForkName));
    }
    if (size) {
      *size = ad.resource_length;
    }
    if (physical_size) {
      *physical_size = ad.resource_length;
    }
    return noErr;
  }
  return errFSNoMoreItems;
}

int FSGetDataForkName(HFSUniStr255* name) {
  name->length = 0;
  return noErr;
}

int FSGetResourceForkName(HFSUniStr255* name) {
  name->length = RESOURCE_FORK_NAME_LENGTH;
  memcpy(name->unicode, kResourceForkName, sizeof(kResourceForkName));
  return noErr;
}

// ---------------------------------------------------------------------------
// Classic file calls

static OSErr classic_error(OSErr err) {
  return err == errFSBadForkRef ? rfNumErr : err;
}

int FSpOpenDF(const FSSpec* spec, SInt8 permission, SInt16* refnum) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  return err ? err : open_fork(path, 0, permission, refnum);
}

int FSpOpenRF(const FSSpec* spec, SInt8 permission, SInt16* refnum) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  return err ? err : open_fork(path, 1, permission, refnum);
}

int FSRead(SInt16 refnum, int32_t* count, void* buffer) {
  ByteCount actual = 0;
  OSErr err = FSReadFork(refnum, fsAtMark, 0, *count > 0 ? *count : 0, buffer,
                         &actual);
  *count = actual;
  return classic_error(err);
}

int FSWrite(SInt16 refnum, int32_t* count, const void* buffer) {
  ByteCount actual = 0;
  OSErr err = FSWriteFork(refnum, fsAtMark, 0, *count > 0 ? *count : 0, buffer,
                          &actual);
  *count = actual;
  return classic_error(err);
}

int FSClose(SInt16 refnum) {
  return classic_error(FSCloseFork(refnum));
}

int GetEOF(SInt16 refnum, int32_t* eof) {
  SInt64 size = 0;
  OSErr err = FSGetForkSize(refnum, &size);
  *eof = size > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)size;
  return classic_error(err);
}

int SetEOF(SInt16 refnum, int32_t eof) {
  return classic_error(FSSetForkSize(refnum, fsFromStart, eof));
}

int SetFPos(SInt16 refnum, SInt16 mode, int32_t offset) {
  return classic_error(FSSetForkPosition(refnum, mode, offset));
}

int GetFPos(SInt16 refnum, int32_t* position) {
  SInt64 mark = 0;
  OSErr err = FSGetForkPosition(refnum, &mark);
  *position = mark > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)mark;
  return classic_error(err);
}

// ---------------------------------------------------------------------------
// For the Resource Manager

OSErr hle_resource_fork_load(const char* path, uint8_t** data,
                             uint32_t* length) {
  hle_appledouble ad;
  if (!hle_appledouble_read(path, &ad) || !ad.has_resource_fork ||
      ad.resource_length == 0) {
    return eofErr;
  }
  int fd = open(ad.path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return hle_oserr_from_errno(errno);
  }
  uint8_t* buffer = malloc(ad.resource_length);
  ssize_t n = buffer ? pread(fd, buffer, ad.resource_length, ad.resource_offset)
                     : -1;
  close(fd);
  if (n != (ssize_t)ad.resource_length) {
    free(buffer);
    return n < 0 && !buffer ? memFullErr : ioErr;
  }
  *data = buffer;
  *length = ad.resource_length;
  return noErr;
}
