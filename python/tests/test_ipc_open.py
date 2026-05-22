"""Integration test: open/close round-trip + slot claim/release.

Gated on the bpServer being available (i.e. running inside the
HMI container with --ipc=host).  Skipped if /dev/shm/bpShmem
isn't present so the rest of the test suite can run in CI.

SPDX-License-Identifier: MIT
"""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import bpclient  # noqa: E402

HAVE_BPSERVER = os.path.exists("/dev/shm/bpShmem")


@pytest.mark.skipif(not HAVE_BPSERVER, reason="needs /dev/shm/bpShmem from bpServer")
def test_open_close_idempotent():
    c = bpclient.Client()
    c.open()
    c.close()
    c.close()  # idempotent


@pytest.mark.skipif(not HAVE_BPSERVER, reason="needs /dev/shm/bpShmem from bpServer")
def test_context_manager_session():
    with bpclient.Client() as c:
        session = c.open_session()
        assert session != 0


@pytest.mark.skipif(not HAVE_BPSERVER, reason="needs /dev/shm/bpShmem from bpServer")
def test_concurrent_slot_claim_releases_cleanly():
    """16 sequential calls should each claim + release a slot
    without leaking slot ownership."""
    with bpclient.Client() as c:
        c.open_session()
        for _ in range(16):
            # Each call reserves a slot and releases it; a leak
            # would surface as BpNoFreeSlot after 16 iterations.
            c.open_session()
