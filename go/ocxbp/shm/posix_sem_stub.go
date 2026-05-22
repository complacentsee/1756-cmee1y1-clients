//go:build !cgo || !linux

// Stub posixSem for non-cgo / non-Linux builds (Windows dev,
// `go vet` runs without a cross-compiler). The real impl is in
// posix_sem.go behind //go:build linux. At runtime, Open() — and
// therefore every operation — would fail when this stub is linked,
// but the package still builds, vets, and lets unit tests for the
// non-cgo layers (ocxbp/cip, byte encoders) compile.
//
// SPDX-License-Identifier: MIT

package shm

import (
	"errors"
	"time"
)

var errNotImplemented = errors.New("shm: POSIX named semaphores not available (cgo disabled or non-Linux build)")

type posixSem struct{}

func semOpen(name string) (*posixSem, error)       { return nil, errNotImplemented }
func (*posixSem) Close()                            {}
func (*posixSem) Post() error                       { return errNotImplemented }
func (*posixSem) Wait() error                       { return errNotImplemented }
func (*posixSem) TryWait() error                    { return errNotImplemented }
func (*posixSem) TimedWaitAbs(deadline time.Time) error { return errNotImplemented }
func (*posixSem) Drain()                            {}
