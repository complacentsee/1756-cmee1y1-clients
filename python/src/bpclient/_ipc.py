"""POSIX shared-memory + named-semaphore wire layer.

Opens /dev/shm/bpShmem and 33 named sems (/bpShm, /bpReqNN,
/bpRespNN) via posix_ipc; mmap.mmap'd onto the SHM region.
Slot claim = cross-process /bpShm sem + in-process threading.Lock.

SPDX-License-Identifier: MIT
"""
import ctypes
import mmap
import os
import struct
import threading
import time
from contextlib import contextmanager

import posix_ipc

from . import _proto as P
from .errors import (
    BP_ERR_CLIENT_OPEN,
    BP_ERR_NO_FREE_SLOT,
    BP_ERR_RECV_ANSWER,
    BP_ERR_SEND_REQUEST,
    BpClientOpen,
    BpEngine,
    BpNoFreeSlot,
    BpRecvAnswer,
    BpSendRequest,
)

# Linux libc.gettid() — added in glibc 2.30 (Aug 2019).  Debian 12's
# glibc 2.36 has it.  Avoids hard-coding arch-specific SYS_gettid.
_libc = ctypes.CDLL("libc.so.6", use_errno=True)
_libc.gettid.restype = ctypes.c_int


def gettid() -> int:
    return _libc.gettid()


class Client:
    """Raw IPC handle to the bpServer.  Opened with open(); released
    with close().  Use the higher-level bpclient.Client (Session,
    TagDB, MessageSend, …) instead of this for application code.
    """

    def __init__(self) -> None:
        self._shm_fd: int | None = None
        self._shm: mmap.mmap | None = None
        self._sem_lock: posix_ipc.Semaphore | None = None
        self._sem_req: list[posix_ipc.Semaphore | None] = [None] * P.SLOT_COUNT
        self._sem_resp: list[posix_ipc.Semaphore | None] = [None] * P.SLOT_COUNT
        self._pid: int = os.getpid()
        self._scan_lock = threading.Lock()

    @property
    def pid(self) -> int:
        return self._pid

    def open(self) -> None:
        """Maps SHM + opens all 33 named semaphores.  Idempotent if
        the client is already open."""
        if self._shm is not None:
            return
        try:
            self._shm_fd = os.open(P.SHM_PATH, os.O_RDWR)
            self._shm = mmap.mmap(self._shm_fd, P.SHM_TOTAL_SIZE,
                                  mmap.MAP_SHARED,
                                  mmap.PROT_READ | mmap.PROT_WRITE)
            self._sem_lock = posix_ipc.Semaphore(P.SEM_SHMLOCK)
            for i in range(P.SLOT_COUNT):
                self._sem_req[i] = posix_ipc.Semaphore(P.SEM_REQ_FMT.format(i))
                self._sem_resp[i] = posix_ipc.Semaphore(P.SEM_RESP_FMT.format(i))
        except Exception as e:
            self.close()
            raise BpClientOpen(f"shm_open/mmap/sem_open failed: {e}") from e

    def close(self) -> None:
        """Releases SHM + all semaphores.  Safe to call multiple times."""
        for i in range(P.SLOT_COUNT):
            if self._sem_req[i] is not None:
                try:
                    self._sem_req[i].close()
                except Exception:
                    pass
                self._sem_req[i] = None
            if self._sem_resp[i] is not None:
                try:
                    self._sem_resp[i].close()
                except Exception:
                    pass
                self._sem_resp[i] = None
        if self._sem_lock is not None:
            try:
                self._sem_lock.close()
            except Exception:
                pass
            self._sem_lock = None
        if self._shm is not None:
            try:
                self._shm.close()
            except Exception:
                pass
            self._shm = None
        if self._shm_fd is not None:
            try:
                os.close(self._shm_fd)
            except Exception:
                pass
            self._shm_fd = None

    def __enter__(self) -> "Client":
        self.open()
        return self

    def __exit__(self, *args) -> None:
        self.close()

    # ----- Slot access helpers -----

    def slot_view(self, idx: int) -> memoryview:
        """Return a memoryview onto slot `idx`'s SLOT_STRIDE bytes.
        Caller must not retain the view past a slot release."""
        assert self._shm is not None
        start = idx * P.SLOT_STRIDE
        return memoryview(self._shm)[start:start + P.SLOT_STRIDE]

    def _drain(self, sem: posix_ipc.Semaphore) -> None:
        """Consume pending posts on a sem without blocking."""
        while True:
            try:
                sem.acquire(0)
            except posix_ipc.BusyError:
                return
            except Exception:
                return

    def _reserve_slot(self, tid: int) -> int:
        """Claim the first free slot.  Returns the index or raises
        BpNoFreeSlot.  Mirrors reserve_slot() in c/src/client.c."""
        assert self._sem_lock is not None and self._shm is not None
        self._sem_lock.acquire()
        try:
            with self._scan_lock:
                for i in range(P.SLOT_COUNT):
                    start = i * P.SLOT_STRIDE + P.HDR_SLOT_OWNER
                    owner = struct.unpack_from("<Q", self._shm, start)[0]
                    if owner == 0:
                        new_owner = ((tid & 0xFFFFFFFF) << 32) | (self._pid & 0xFFFFFFFF)
                        struct.pack_into("<Q", self._shm, start, new_owner)
                        self._drain(self._sem_req[i])
                        self._drain(self._sem_resp[i])
                        return i
        finally:
            self._sem_lock.release()
        raise BpNoFreeSlot("all 16 slots in use")

    def _release_slot(self, idx: int) -> None:
        """Zero the owner field and drain stale posts.  Mirrors
        release_slot() in c/src/client.c."""
        assert self._sem_lock is not None and self._shm is not None
        try:
            self._sem_lock.acquire()
            start = idx * P.SLOT_STRIDE + P.HDR_SLOT_OWNER
            struct.pack_into("<Q", self._shm, start, 0)
            self._drain(self._sem_req[idx])
            self._drain(self._sem_resp[idx])
        finally:
            self._sem_lock.release()

    # ----- Call dispatcher -----

    def call(self, fn_name: str, payload_size: int,
             fill=None, read=None, timeout_ms: int = 30000) -> None:
        """Perform one OCXcip_* slot round-trip.  Mirrors
        bp_client_call in c/src/client.c.

        - fill(slot: memoryview) writes the per-opcode request bytes
        - read(slot: memoryview) reads the per-opcode response bytes
        Either or both may be None.

        Raises:
          BpSendRequest  if sem_post on bpReqN fails
          BpRecvAnswer   if sem_timedwait on bpRespN times out and
                         the errorcode-poll fallback also gives up
          BpEngine       if the engine wrote a non-zero errorcode
                         (transport-level rc, not CIP General Status)
          BpNoFreeSlot   if no slot is free across all 16
        """
        assert self._shm is not None
        tid = gettid()
        idx = self._reserve_slot(tid)
        try:
            slot = self.slot_view(idx)

            # Header
            struct.pack_into("<HHI", slot, P.HDR_OPCODE,
                             P.OPCODE_CIP, 0, payload_size)
            # fn_name: 63 bytes + NUL terminator at +0x47
            fn_bytes = fn_name.encode("ascii", "strict")[:63]
            slot[P.HDR_FN_NAME:P.HDR_FN_NAME + 64] = b"\x00" * 64
            slot[P.HDR_FN_NAME:P.HDR_FN_NAME + len(fn_bytes)] = fn_bytes

            struct.pack_into("<I", slot, P.HDR_CLIENT_PID, self._pid)
            struct.pack_into("<HH", slot, P.HDR_IS_DOCKER, 1, 0)  # always 1 from this SDK
            struct.pack_into("<II", slot, P.HDR_ERRORCODE, P.PENDING_ERROR_BITS, 0)
            # slot_owner already set by _reserve_slot
            struct.pack_into("<II", slot, P.HDR_SLOT_NUMBER, idx, 0)
            slot[0x68:0x78] = b"\x00" * 16
            # Zero the entire payload+response area so unwritten
            # fields don't read stale neighbor data.
            slot[P.HDR_PAYLOAD_START:P.SLOT_STRIDE] = b"\x00" * (P.SLOT_STRIDE - P.HDR_PAYLOAD_START)

            if fill is not None:
                fill(slot)

            try:
                self._sem_req[idx].release()
            except Exception as e:
                raise BpSendRequest(f"sem_post on bpReq{idx:02d} failed: {e}") from e

            timeout_s = (timeout_ms if timeout_ms > 0 else 30000) / 1000.0
            try:
                self._sem_resp[idx].acquire(timeout_s)
                got_reply = True
            except posix_ipc.BusyError:
                got_reply = False

            if not got_reply:
                # Fallback: poll errorcode for 200 ms (matches C SDK).
                deadline = time.monotonic() + 0.200
                while time.monotonic() < deadline:
                    ec = struct.unpack_from("<I", slot, P.HDR_ERRORCODE)[0]
                    if ec != P.PENDING_ERROR_BITS:
                        got_reply = True
                        break
                    time.sleep(0.002)
                if not got_reply:
                    raise BpRecvAnswer("sem_wait on bpResp timed out")

            errcode_u32 = struct.unpack_from("<I", slot, P.HDR_ERRORCODE)[0]
            errcode_i32 = errcode_u32 if errcode_u32 < 0x80000000 else errcode_u32 - 0x100000000
            if errcode_i32 != 0:
                raise BpEngine(errcode_i32)

            if read is not None:
                read(slot)
        finally:
            self._release_slot(idx)


@contextmanager
def open_client():
    """Context manager: yields an opened Client; closes on exit."""
    c = Client()
    c.open()
    try:
        yield c
    finally:
        c.close()
