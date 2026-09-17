// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Multiprocessing Services over pthreads: tasks, message queues,
// semaphores, event groups and critical regions, and the atomic arithmetic
// of Driver Services and Open Transport.
//
// A task runs its entry point under sigsetjmp, so MPExit and MPTerminateTask
// leave it with siglongjmp. glibc's pthread_exit would unwind instead, and
// the Mach-O frames on the stack carry no unwind tables it could follow.
// Termination is cooperative: a task told to stop leaves at its next MP
// call, as waits wake up often enough to notice.

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "carbon.h"

enum {
  kMPDeletedErr = -29295,
  kMPTimeoutErr = -29296,
  kMPInsufficientResourcesErr = -29298,
  kMPInvalidIDErr = -29299,
};

enum {
  kDurationImmediate = 0,
  kDurationForever = 0x7FFFFFFF,
};

enum {
  kMPAllocateClearMask = 0x0001,
};

enum {
  kMagicTask = 'MPtk',
  kMagicQueue = 'MPqu',
  kMagicSemaphore = 'MPsm',
  kMagicEvent = 'MPev',
  kMagicCriticalRegion = 'MPcr',
};

// How long a wait sleeps before it looks for a termination request.
static const uint64_t kWaitSliceNs = 50 * 1000 * 1000;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

// A Duration counts milliseconds when positive, microseconds when negative.
static uint64_t deadline_after(int32_t duration) {
  if (duration == kDurationForever) {
    return UINT64_MAX;
  }
  uint64_t ns = duration >= 0 ? (uint64_t)duration * 1000000u
                              : (uint64_t)(-(int64_t)duration) * 1000u;
  return now_ns() + ns;
}

static void init_cond(pthread_cond_t* cond) {
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(cond, &attr);
  pthread_condattr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// Tasks

typedef struct mp_queue mp_queue;

typedef struct mp_task {
  uint32_t magic;
  int (*entry)(void* parameter);
  void* parameter;
  mp_queue* notify;
  void* termination1;
  void* termination2;
  volatile int terminate;
  int32_t exit_status;
  sigjmp_buf exit_jump;
} mp_task;

static mp_task main_task = { .magic = kMagicTask };
static __thread mp_task* current_task;

static mp_task* this_task(void) {
  return current_task ? current_task : &main_task;
}

static int terminating(void) {
  return current_task && current_task->terminate;
}

// Leaves the current task if it has been told to stop. Never call it with
// a lock held.
static void checkpoint(void) {
  if (terminating()) {
    siglongjmp(current_task->exit_jump, 1);
  }
}

// Waits on |cond| for at most one slice, never past |deadline|. Returns 0
// when woken or the slice ran out, ETIMEDOUT once the deadline has passed.
static int wait_slice(pthread_cond_t* cond, pthread_mutex_t* lock,
                      uint64_t deadline) {
  uint64_t now = now_ns();
  if (now >= deadline) {
    return ETIMEDOUT;
  }
  uint64_t until = deadline - now > kWaitSliceNs ? now + kWaitSliceNs
                                                 : deadline;
  struct timespec ts = { until / 1000000000u, until % 1000000000u };
  pthread_cond_timedwait(cond, lock, &ts);
  return 0;
}

static void queue_post(mp_queue* queue, void* p1, void* p2, void* p3);

static void* task_main(void* arg) {
  mp_task* task = arg;
  current_task = task;
  int32_t status;
  if (sigsetjmp(task->exit_jump, 0) == 0) {
    status = task->entry(task->parameter);
  } else {
    status = task->exit_status;
  }
  if (task->notify) {
    queue_post(task->notify, task->termination1, task->termination2,
               (void*)(intptr_t)status);
  }
  // The ID stays valid after the task ends, as the game may still name it.
  task->magic = kMagicTask;
  return NULL;
}

int MPCreateTask(int (*entry)(void*), void* parameter, uint32_t stack_size,
                 mp_queue* notify, void* termination1, void* termination2,
                 uint32_t options, mp_task** out) {
  if (!entry || !out) {
    return paramErr;
  }
  mp_task* task = calloc(1, sizeof(*task));
  task->magic = kMagicTask;
  task->entry = entry;
  task->parameter = parameter;
  task->notify = notify;
  task->termination1 = termination1;
  task->termination2 = termination2;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  size_t stack = stack_size < 1024 * 1024 ? 1024 * 1024 : stack_size;
  pthread_attr_setstacksize(&attr, stack);
  pthread_t thread;
  int err = pthread_create(&thread, &attr, task_main, task);
  pthread_attr_destroy(&attr);
  if (err) {
    free(task);
    return kMPInsufficientResourcesErr;
  }
  *out = task;
  return noErr;
}

int MPTerminateTask(mp_task* task, int32_t status) {
  if (!task || task->magic != kMagicTask) {
    return kMPInvalidIDErr;
  }
  if (task == &main_task) {
    return paramErr;
  }
  task->exit_status = status;
  task->terminate = 1;
  if (task == current_task) {
    checkpoint();
  }
  return noErr;
}

void MPExit(int32_t status) {
  if (!current_task) {
    exit(status);
  }
  current_task->exit_status = status;
  siglongjmp(current_task->exit_jump, 1);
}

mp_task* MPCurrentTaskID(void) {
  return this_task();
}

int MPSetTaskWeight(mp_task* task, uint32_t weight) {
  return task && task->magic == kMagicTask ? noErr : kMPInvalidIDErr;
}

unsigned char MPTaskIsPreemptive(mp_task* task) {
  return 1;
}

int MPYield(void) {
  checkpoint();
  sched_yield();
  return noErr;
}

// AbsoluteTime is mach_absolute_time's count, nanoseconds here.
int MPDelayUntil(const uint64_t* expiration) {
  for (;;) {
    checkpoint();
    uint64_t now = now_ns();
    if (now >= *expiration) {
      return noErr;
    }
    uint64_t wait = *expiration - now;
    if (current_task && wait > kWaitSliceNs) {
      wait = kWaitSliceNs;
    }
    struct timespec ts = { wait / 1000000000u, wait % 1000000000u };
    nanosleep(&ts, NULL);
  }
}

uint32_t MPProcessors(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? n : 1;
}

uint32_t MPProcessorsScheduled(void) {
  return MPProcessors();
}

// ---------------------------------------------------------------------------
// Message queues

typedef struct mp_message {
  void* p[3];
  struct mp_message* next;
} mp_message;

struct mp_queue {
  uint32_t magic;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  mp_message* head;
  mp_message* tail;
  int deleted;
};

static void queue_post(mp_queue* queue, void* p1, void* p2, void* p3) {
  mp_message* m = malloc(sizeof(*m));
  m->p[0] = p1;
  m->p[1] = p2;
  m->p[2] = p3;
  m->next = NULL;
  pthread_mutex_lock(&queue->lock);
  if (queue->tail) {
    queue->tail->next = m;
  } else {
    queue->head = m;
  }
  queue->tail = m;
  pthread_cond_signal(&queue->cond);
  pthread_mutex_unlock(&queue->lock);
}

int MPCreateQueue(mp_queue** out) {
  if (!out) {
    return paramErr;
  }
  mp_queue* queue = calloc(1, sizeof(*queue));
  queue->magic = kMagicQueue;
  pthread_mutex_init(&queue->lock, NULL);
  init_cond(&queue->cond);
  *out = queue;
  return noErr;
}

// Waiters may still hold the queue, so its memory is kept.
int MPDeleteQueue(mp_queue* queue) {
  if (!queue || queue->magic != kMagicQueue) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&queue->lock);
  queue->deleted = 1;
  queue->magic = 0;
  while (queue->head) {
    mp_message* m = queue->head;
    queue->head = m->next;
    free(m);
  }
  queue->tail = NULL;
  pthread_cond_broadcast(&queue->cond);
  pthread_mutex_unlock(&queue->lock);
  return noErr;
}

int MPNotifyQueue(mp_queue* queue, void* p1, void* p2, void* p3) {
  if (!queue || queue->magic != kMagicQueue) {
    return kMPInvalidIDErr;
  }
  queue_post(queue, p1, p2, p3);
  return noErr;
}

int MPWaitOnQueue(mp_queue* queue, void** p1, void** p2, void** p3,
                  int32_t timeout) {
  if (!queue || queue->magic != kMagicQueue) {
    return kMPInvalidIDErr;
  }
  uint64_t deadline = deadline_after(timeout);
  pthread_mutex_lock(&queue->lock);
  int err = noErr;
  while (!queue->head) {
    if (queue->deleted) {
      err = kMPDeletedErr;
      break;
    }
    if (terminating()) {
      pthread_mutex_unlock(&queue->lock);
      checkpoint();
    }
    if (wait_slice(&queue->cond, &queue->lock, deadline) == ETIMEDOUT) {
      err = kMPTimeoutErr;
      break;
    }
  }
  if (err == noErr) {
    mp_message* m = queue->head;
    queue->head = m->next;
    if (!queue->head) {
      queue->tail = NULL;
    }
    if (p1) {
      *p1 = m->p[0];
    }
    if (p2) {
      *p2 = m->p[1];
    }
    if (p3) {
      *p3 = m->p[2];
    }
    free(m);
  }
  pthread_mutex_unlock(&queue->lock);
  return err;
}

// ---------------------------------------------------------------------------
// Semaphores

typedef struct {
  uint32_t magic;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint32_t count;
  uint32_t maximum;
  int deleted;
} mp_semaphore;

int MPCreateSemaphore(uint32_t maximum, uint32_t initial, mp_semaphore** out) {
  if (!out || initial > maximum) {
    return paramErr;
  }
  mp_semaphore* s = calloc(1, sizeof(*s));
  s->magic = kMagicSemaphore;
  pthread_mutex_init(&s->lock, NULL);
  init_cond(&s->cond);
  s->count = initial;
  s->maximum = maximum;
  *out = s;
  return noErr;
}

int MPDeleteSemaphore(mp_semaphore* s) {
  if (!s || s->magic != kMagicSemaphore) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&s->lock);
  s->deleted = 1;
  s->magic = 0;
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->lock);
  return noErr;
}

int MPSignalSemaphore(mp_semaphore* s) {
  if (!s || s->magic != kMagicSemaphore) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&s->lock);
  int err = noErr;
  if (s->count < s->maximum) {
    s->count++;
    pthread_cond_signal(&s->cond);
  } else {
    err = kMPInsufficientResourcesErr;
  }
  pthread_mutex_unlock(&s->lock);
  return err;
}

int MPWaitOnSemaphore(mp_semaphore* s, int32_t timeout) {
  if (!s || s->magic != kMagicSemaphore) {
    return kMPInvalidIDErr;
  }
  uint64_t deadline = deadline_after(timeout);
  pthread_mutex_lock(&s->lock);
  int err = noErr;
  while (s->count == 0) {
    if (s->deleted) {
      err = kMPDeletedErr;
      break;
    }
    if (terminating()) {
      pthread_mutex_unlock(&s->lock);
      checkpoint();
    }
    if (wait_slice(&s->cond, &s->lock, deadline) == ETIMEDOUT) {
      err = kMPTimeoutErr;
      break;
    }
  }
  if (err == noErr) {
    s->count--;
  }
  pthread_mutex_unlock(&s->lock);
  return err;
}

// ---------------------------------------------------------------------------
// Event groups: 32 flag bits a waiter collects and clears.

typedef struct {
  uint32_t magic;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint32_t flags;
  int deleted;
} mp_event;

int MPCreateEvent(mp_event** out) {
  if (!out) {
    return paramErr;
  }
  mp_event* e = calloc(1, sizeof(*e));
  e->magic = kMagicEvent;
  pthread_mutex_init(&e->lock, NULL);
  init_cond(&e->cond);
  *out = e;
  return noErr;
}

int MPDeleteEvent(mp_event* e) {
  if (!e || e->magic != kMagicEvent) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&e->lock);
  e->deleted = 1;
  e->magic = 0;
  pthread_cond_broadcast(&e->cond);
  pthread_mutex_unlock(&e->lock);
  return noErr;
}

int MPSetEvent(mp_event* e, uint32_t flags) {
  if (!e || e->magic != kMagicEvent) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&e->lock);
  e->flags |= flags;
  pthread_cond_broadcast(&e->cond);
  pthread_mutex_unlock(&e->lock);
  return noErr;
}

int MPWaitForEvent(mp_event* e, uint32_t* flags, int32_t timeout) {
  if (!e || e->magic != kMagicEvent) {
    return kMPInvalidIDErr;
  }
  uint64_t deadline = deadline_after(timeout);
  pthread_mutex_lock(&e->lock);
  int err = noErr;
  while (e->flags == 0) {
    if (e->deleted) {
      err = kMPDeletedErr;
      break;
    }
    if (terminating()) {
      pthread_mutex_unlock(&e->lock);
      checkpoint();
    }
    if (wait_slice(&e->cond, &e->lock, deadline) == ETIMEDOUT) {
      err = kMPTimeoutErr;
      break;
    }
  }
  if (flags) {
    *flags = e->flags;
  }
  e->flags = 0;
  pthread_mutex_unlock(&e->lock);
  return err;
}

// ---------------------------------------------------------------------------
// Critical regions: a lock the thread holding it may enter again, released
// when it has exited as many times. The owner is a thread, not an MP task,
// as the game enters them from threads it made with pthread_create too.

typedef struct {
  uint32_t magic;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t owner;
  uint32_t depth;
  int deleted;
} mp_critical_region;

int MPCreateCriticalRegion(mp_critical_region** out) {
  if (!out) {
    return paramErr;
  }
  mp_critical_region* r = calloc(1, sizeof(*r));
  r->magic = kMagicCriticalRegion;
  pthread_mutex_init(&r->lock, NULL);
  init_cond(&r->cond);
  *out = r;
  return noErr;
}

int MPDeleteCriticalRegion(mp_critical_region* r) {
  if (!r || r->magic != kMagicCriticalRegion) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&r->lock);
  r->deleted = 1;
  r->magic = 0;
  pthread_cond_broadcast(&r->cond);
  pthread_mutex_unlock(&r->lock);
  return noErr;
}

int MPEnterCriticalRegion(mp_critical_region* r, int32_t timeout) {
  if (!r || r->magic != kMagicCriticalRegion) {
    return kMPInvalidIDErr;
  }
  pthread_t self = pthread_self();
  pthread_mutex_lock(&r->lock);
  int err = noErr;
  if (r->depth && pthread_equal(r->owner, self)) {
    r->depth++;
    pthread_mutex_unlock(&r->lock);
    return noErr;
  }
  uint64_t deadline = deadline_after(timeout);
  while (r->depth) {
    if (r->deleted) {
      err = kMPDeletedErr;
      break;
    }
    if (terminating()) {
      pthread_mutex_unlock(&r->lock);
      checkpoint();
    }
    if (wait_slice(&r->cond, &r->lock, deadline) == ETIMEDOUT) {
      err = kMPTimeoutErr;
      break;
    }
  }
  if (err == noErr) {
    r->owner = self;
    r->depth = 1;
  }
  pthread_mutex_unlock(&r->lock);
  return err;
}

int MPExitCriticalRegion(mp_critical_region* r) {
  if (!r || r->magic != kMagicCriticalRegion) {
    return kMPInvalidIDErr;
  }
  pthread_mutex_lock(&r->lock);
  int err = noErr;
  if (!r->depth || !pthread_equal(r->owner, pthread_self())) {
    // Only the thread holding the region can leave it.
    err = kMPInsufficientResourcesErr;
  } else if (--r->depth == 0) {
    pthread_cond_signal(&r->cond);
  }
  pthread_mutex_unlock(&r->lock);
  return err;
}

// ---------------------------------------------------------------------------
// Aligned allocation

// |alignment| is a power of two's exponent; 254 asks for a page and 255 for
// what atomic operations need.
void* MPAllocateAligned(uint32_t size, uint8_t alignment, uint32_t options) {
  size_t align;
  if (alignment == 254) {
    align = sysconf(_SC_PAGESIZE);
  } else if (alignment == 0 || alignment == 255 || alignment > 16) {
    align = 16;
  } else {
    align = (size_t)1 << alignment;
  }
  if (align < sizeof(void*)) {
    align = sizeof(void*);
  }
  void* p;
  if (posix_memalign(&p, align, size ? size : 1)) {
    return NULL;
  }
  if (options & kMPAllocateClearMask) {
    memset(p, 0, size);
  }
  return p;
}

void MPFree(void* p) {
  free(p);
}

// ---------------------------------------------------------------------------
// Atomic arithmetic. Each returns the value it stored.

int32_t IncrementAtomic(int32_t* value) {
  return __sync_add_and_fetch(value, 1);
}

int32_t DecrementAtomic(int32_t* value) {
  return __sync_sub_and_fetch(value, 1);
}

int32_t OTAtomicAdd32(int32_t amount, int32_t* value) {
  return __sync_add_and_fetch(value, amount);
}

// A Boolean result fills EAX.
unsigned int CompareAndSwap(uint32_t old_value, uint32_t new_value,
                            uint32_t* address) {
  return __sync_bool_compare_and_swap(address, old_value, new_value);
}
