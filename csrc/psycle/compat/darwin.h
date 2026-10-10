// SPDX-License-Identifier: GPL-3.0-or-later
// What Psycle's universalis asks of GNU/Linux and macOS lacks, included on macOS only:
// the sincos family (Apple has __sincos and __sincosf, nothing for long double), and the
// CPU-affinity and real-time scheduling calls. The player renders headless and never
// pins a thread: the affinity is every processor, and setting it does nothing.
#pragma once
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static inline void sincos(double x, double* s, double* c) { __sincos(x, s, c); }
static inline void sincosf(float x, float* s, float* c) { __sincosf(x, s, c); }
static inline void sincosl(long double x, long double* s, long double* c) { *s = sinl(x); *c = cosl(x); }

typedef struct { uint64_t bits[16]; } cpu_set_t;
#define CPU_SETSIZE 1024
#define CPU_ZERO(s) memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(i, s) ((s)->bits[(i) / 64] |= (1ULL << ((i) % 64)))
#define CPU_CLR(i, s) ((s)->bits[(i) / 64] &= ~(1ULL << ((i) % 64)))
#define CPU_ISSET(i, s) (((s)->bits[(i) / 64] >> ((i) % 64)) & 1)

static inline int sched_getaffinity(pid_t, size_t, cpu_set_t* m) {
  CPU_ZERO(m);
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  for (long i = 0; i < n && i < CPU_SETSIZE; i++) CPU_SET(i, m);
  return 0;
}
static inline int sched_setaffinity(pid_t, size_t, const cpu_set_t*) { return 0; }
static inline int pthread_getaffinity_np(pthread_t, size_t n, cpu_set_t* m) { return sched_getaffinity(0, n, m); }
static inline int pthread_setaffinity_np(pthread_t, size_t, const cpu_set_t*) { return 0; }
static inline int sched_setscheduler(pid_t, int, const struct sched_param*) { return 0; }

// the process's processor-time clock (macOS has the clock, not this way to ask for it)
static inline int clock_getcpuclockid(pid_t, clockid_t* id) { *id = CLOCK_PROCESS_CPUTIME_ID; return 0; }
