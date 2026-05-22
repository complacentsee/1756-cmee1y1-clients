//go:build linux

// POSIX named-semaphore wrappers (cgo).
//
// `golang.org/x/sys/unix` does not expose sem_open / sem_post /
// sem_wait / sem_timedwait, and the bpServer transport hard-requires
// POSIX named sems. Reimplementing them via direct futex on the
// glibc sem_t internals is fragile across glibc versions, so we
// keep this ~30-line cgo shim. See memory:go-port-cgo-sems for the
// agreed-upon scope of cgo in this SDK.
//
// SPDX-License-Identifier: MIT

package shm

// #cgo LDFLAGS: -lpthread
// #include <errno.h>
// #include <fcntl.h>
// #include <semaphore.h>
// #include <stdlib.h>
// #include <time.h>
//
// static sem_t *go_sem_open(const char *name, int *out_errno) {
//     sem_t *r = sem_open(name, 0);
//     if (r == SEM_FAILED) {
//         *out_errno = errno;
//         return 0;
//     }
//     *out_errno = 0;
//     return r;
// }
// static int go_sem_close(sem_t *s) {
//     if (sem_close(s) == 0) return 0;
//     return -errno;
// }
// static int go_sem_post(sem_t *s) {
//     if (sem_post(s) == 0) return 0;
//     return -errno;
// }
// static int go_sem_wait(sem_t *s) {
//     while (1) {
//         if (sem_wait(s) == 0) return 0;
//         if (errno == EINTR) continue;
//         return -errno;
//     }
// }
// static int go_sem_trywait(sem_t *s) {
//     if (sem_trywait(s) == 0) return 0;
//     return -errno;
// }
// static int go_sem_timedwait(sem_t *s, long long abs_nanos) {
//     struct timespec ts;
//     ts.tv_sec  = (time_t)(abs_nanos / 1000000000LL);
//     ts.tv_nsec = (long)(abs_nanos % 1000000000LL);
//     while (1) {
//         if (sem_timedwait(s, &ts) == 0) return 0;
//         if (errno == EINTR) continue;
//         return -errno;
//     }
// }
import "C"

import (
	"fmt"
	"syscall"
	"time"
	"unsafe"
)

// posixSem holds an open named-semaphore handle (sem_t*).
type posixSem struct {
	ptr *C.sem_t
}

func semOpen(name string) (*posixSem, error) {
	cname := C.CString(name)
	defer C.free(unsafe.Pointer(cname))
	var cerrno C.int
	p := C.go_sem_open(cname, &cerrno)
	if p == nil {
		return nil, fmt.Errorf("sem_open(%s): %w", name, syscall.Errno(int(cerrno)))
	}
	return &posixSem{ptr: p}, nil
}

func (s *posixSem) Close() {
	if s == nil || s.ptr == nil {
		return
	}
	C.go_sem_close(s.ptr)
	s.ptr = nil
}

func (s *posixSem) Post() error {
	if rc := int(C.go_sem_post(s.ptr)); rc != 0 {
		return syscall.Errno(-rc)
	}
	return nil
}

func (s *posixSem) Wait() error {
	if rc := int(C.go_sem_wait(s.ptr)); rc != 0 {
		return syscall.Errno(-rc)
	}
	return nil
}

func (s *posixSem) TryWait() error {
	if rc := int(C.go_sem_trywait(s.ptr)); rc != 0 {
		return syscall.Errno(-rc)
	}
	return nil
}

// TimedWaitAbs blocks until either the semaphore is posted or the
// absolute CLOCK_REALTIME deadline passes. Returns syscall.ETIMEDOUT
// on timeout.
func (s *posixSem) TimedWaitAbs(deadline time.Time) error {
	if rc := int(C.go_sem_timedwait(s.ptr, C.longlong(deadline.UnixNano()))); rc != 0 {
		return syscall.Errno(-rc)
	}
	return nil
}

// Drain non-blockingly consumes any pending posts on the semaphore.
// Used during slot claim/release to flush stale signals.
func (s *posixSem) Drain() {
	for s.TryWait() == nil {
	}
}
