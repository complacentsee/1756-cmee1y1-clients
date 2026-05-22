// SPDX-License-Identifier: MIT

package cip

import (
	"bytes"
	"testing"
)

func TestBuildUnconnectedSendCanonical(t *testing.T) {
	// Reproduces the canonical example in docs/protocol.md.
	embedded := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	route := []byte{0x02, 0x01}
	buf := make([]byte, 64)
	n := BuildUnconnectedSend(buf, embedded, route, 5024)
	if n <= 0 {
		t.Fatalf("BuildUnconnectedSend rc=%d", n)
	}
	got := buf[:n]
	// ticks = ceil(5024 / 32) = 157 = 0x9D
	want := []byte{
		0x52, 0x02,
		0x20, 0x06, 0x24, 0x01,
		0x05, 0x9D,
		0x06, 0x00,
		0x01, 0x02, 0x20, 0x01, 0x24, 0x01,
		0x01, 0x00,
		0x02, 0x01,
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("got %x, want %x", got, want)
	}
}

func TestBuildUnconnectedSendPadsOddEmbedded(t *testing.T) {
	embedded := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE}
	route := []byte{0x01, 0x02}
	buf := make([]byte, 64)
	n := BuildUnconnectedSend(buf, embedded, route, 5000)
	if n <= 0 {
		t.Fatalf("rc=%d", n)
	}
	out := buf[:n]
	if out[15] != 0x00 {
		t.Errorf("expected pad byte at offset 15, got 0x%02x", out[15])
	}
	if out[16] != 1 || out[17] != 0x00 {
		t.Errorf("route header wrong: %x", out[16:18])
	}
	if !bytes.Equal(out[18:20], route) {
		t.Errorf("route bytes wrong: %x", out[18:20])
	}
}

func TestBuildUnconnectedSendClampsTicks(t *testing.T) {
	embedded := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	route := []byte{0x01, 0x02}
	buf := make([]byte, 64)
	// timeout 1 ms → ticks = 1
	n := BuildUnconnectedSend(buf, embedded, route, 1)
	if buf[7] != 1 {
		t.Errorf("min timeout tick: got %d, want 1", buf[7])
	}
	// Huge timeout → clamps to 255
	n = BuildUnconnectedSend(buf, embedded, route, 60000)
	if buf[7] != 255 {
		t.Errorf("max timeout tick: got %d, want 255", buf[7])
	}
	_ = n
}

func TestBuildUnconnectedSendRejectsOddRoute(t *testing.T) {
	embedded := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	odd := []byte{0x01, 0x02, 0x02} // 3 bytes — invalid
	buf := make([]byte, 64)
	if rc := BuildUnconnectedSend(buf, embedded, odd, 5000); rc != -22 {
		t.Errorf("expected -22 (EINVAL), got %d", rc)
	}
}

func TestBuildUnconnectedSendBufferTooSmall(t *testing.T) {
	embedded := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	route := []byte{0x01, 0x02}
	buf := make([]byte, 10) // too small for total = 20
	if rc := BuildUnconnectedSend(buf, embedded, route, 5000); rc != -28 {
		t.Errorf("expected -28 (ENOSPC), got %d", rc)
	}
}

func TestAppendPortSegment(t *testing.T) {
	route := make([]byte, 8)
	off := 0
	if !AppendPortSegment(route, &off, 1, 2) {
		t.Fatal("AppendPortSegment 1 failed")
	}
	if !AppendPortSegment(route, &off, 2, 5) {
		t.Fatal("AppendPortSegment 2 failed")
	}
	want := []byte{0x01, 0x02, 0x02, 0x05}
	if !bytes.Equal(route[:off], want) {
		t.Errorf("got %x, want %x", route[:off], want)
	}
}

func TestAppendPortSegmentBufferFull(t *testing.T) {
	route := make([]byte, 1) // not enough for 2 bytes
	off := 0
	if AppendPortSegment(route, &off, 1, 2) {
		t.Error("should have failed on 1-byte buffer")
	}
}
