// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Darwin's pthread condition variables, and the Mach calls that name a
// thread's scheduling.
//
// A Darwin i386 pthread_cond_t is 28 bytes and glibc's is 32, so a game
// structure with a condition variable in it would lose the 4 bytes after
// it to glibc. Each one here holds a glibc condition variable allocated
// on its own, found through the pointer after the signature. One set up
// by PTHREAD_COND_INITIALIZER gets its glibc half on first use.

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  kHleCondSig = 0x486C6543,  // 'HleC'
};

typedef struct {
  int32_t sig;
  pthread_cond_t* cond;
  char opaque[20];
} darwin_cond;

_Static_assert(sizeof(darwin_cond) == 28, "Darwin i386 pthread_cond_t");

static pthread_cond_t* cond_of(darwin_cond* c) {
  pthread_cond_t* cond = c->cond;
  if (cond) {
    return cond;
  }
  pthread_cond_t* made = malloc(sizeof(*made));
  pthread_cond_init(made, NULL);
  cond = __sync_val_compare_and_swap(&c->cond, NULL, made);
  if (cond) {
    // Another thread got there first.
    pthread_cond_destroy(made);
    free(made);
    return cond;
  }
  c->sig = kHleCondSig;
  return made;
}

int __darwin_pthread_cond_init(darwin_cond* c, const void* attr) {
  memset(c, 0, sizeof(*c));
  c->cond = malloc(sizeof(*c->cond));
  if (!c->cond) {
    return ENOMEM;
  }
  pthread_cond_init(c->cond, NULL);
  c->sig = kHleCondSig;
  return 0;
}

int __darwin_pthread_cond_destroy(darwin_cond* c) {
  if (c->cond) {
    pthread_cond_destroy(c->cond);
    free(c->cond);
  }
  memset(c, 0, sizeof(*c));
  return 0;
}

int __darwin_pthread_cond_signal(darwin_cond* c) {
  return pthread_cond_signal(cond_of(c));
}

int __darwin_pthread_cond_broadcast(darwin_cond* c) {
  return pthread_cond_broadcast(cond_of(c));
}

int __darwin_pthread_cond_wait(darwin_cond* c, pthread_mutex_t* mutex) {
  return pthread_cond_wait(cond_of(c), mutex);
}

// Darwin's ETIMEDOUT is 60; glibc's is 110.
enum { kDarwinETIMEDOUT = 60 };

static int darwin_errno(int err) {
  return err == ETIMEDOUT ? kDarwinETIMEDOUT : err;
}

int __darwin_pthread_cond_timedwait(darwin_cond* c, pthread_mutex_t* mutex,
                                    const struct timespec* abstime) {
  return darwin_errno(pthread_cond_timedwait(cond_of(c), mutex, abstime));
}

int pthread_cond_timedwait_relative_np(darwin_cond* c, pthread_mutex_t* mutex,
                                       const struct timespec* reltime) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += reltime->tv_sec;
  until.tv_nsec += reltime->tv_nsec;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
  }
  return darwin_errno(pthread_cond_timedwait(cond_of(c), mutex, &until));
}

// ---------------------------------------------------------------------------
// Mach threads. A thread's port is its pthread_t, which is 32 bits here.

typedef uint32_t mach_port_t;

enum {
  KERN_SUCCESS = 0,
  KERN_INVALID_ARGUMENT = 4,
};

enum {
  THREAD_BASIC_INFO = 3,
  THREAD_BASIC_INFO_COUNT = 10,
  THREAD_SCHED_TIMESHARE_INFO = 10,
  POLICY_TIMESHARE_INFO_COUNT = 5,
  POLICY_TIMESHARE = 1,
  TH_STATE_RUNNING = 1,
};

// The priority Mac OS X gives an ordinary thread.
enum { kDefaultPriority = 31 };

mach_port_t pthread_mach_thread_np(pthread_t thread) {
  return (mach_port_t)thread;
}

typedef struct {
  int32_t seconds;
  int32_t microseconds;
} time_value_t;

typedef struct {
  time_value_t user_time;
  time_value_t system_time;
  int32_t cpu_usage;
  int32_t policy;
  int32_t run_state;
  int32_t flags;
  int32_t suspend_count;
  int32_t sleep_time;
} thread_basic_info;

typedef struct {
  int32_t max_priority;
  int32_t base_priority;
  int32_t cur_priority;
  int32_t depressed;
  int32_t depress_priority;
} policy_timeshare_info;

// Every thread is a time-sharing thread at the default priority: the
// scheduling the game asks for is not applied.
int thread_info(mach_port_t thread, int flavor, int32_t* info,
                uint32_t* count) {
  if (!info || !count) {
    return KERN_INVALID_ARGUMENT;
  }
  switch (flavor) {
    case THREAD_BASIC_INFO: {
      if (*count < THREAD_BASIC_INFO_COUNT) {
        return KERN_INVALID_ARGUMENT;
      }
      thread_basic_info basic = {
        .policy = POLICY_TIMESHARE,
        .run_state = TH_STATE_RUNNING,
      };
      memcpy(info, &basic, sizeof(basic));
      *count = THREAD_BASIC_INFO_COUNT;
      return KERN_SUCCESS;
    }
    case THREAD_SCHED_TIMESHARE_INFO: {
      if (*count < POLICY_TIMESHARE_INFO_COUNT) {
        return KERN_INVALID_ARGUMENT;
      }
      policy_timeshare_info timeshare = {
        .max_priority = 63,
        .base_priority = kDefaultPriority,
        .cur_priority = kDefaultPriority,
      };
      memcpy(info, &timeshare, sizeof(timeshare));
      *count = POLICY_TIMESHARE_INFO_COUNT;
      return KERN_SUCCESS;
    }
  }
  fprintf(stderr, "hle: thread_info flavor %d is not answered\n", flavor);
  return KERN_INVALID_ARGUMENT;
}

int thread_policy_set(mach_port_t thread, int flavor, const int32_t* policy,
                      uint32_t count) {
  return KERN_SUCCESS;
}
