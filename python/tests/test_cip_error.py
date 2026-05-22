"""Unit tests for the structured CIP-layer error type (v0.8.0).

Verifies BpCipError carries the four wire-level fields, the
human-readable message helpers map common (status, ext_status)
pairs to the strings documented in docs/error-codes.md, and
err_code() round-trips the type back to BP_ERR_CIP_STATUS.

Pure unit tests — no PLC, no /dev/shm, no posix_ipc runtime —
runs on any host.

SPDX-License-Identifier: MIT
"""
import importlib.util
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

# Load errors module without dragging in _ipc (posix_ipc is Linux-only).
_spec = importlib.util.spec_from_file_location(
    "bpclient_errors", SRC / "bpclient" / "errors.py")
errors = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(errors)


def test_cip_error_carries_structured_fields():
    e = errors.BpCipError(service=0xDB, status=0x01, ext_status=0x0100, slot=2)
    assert e.service == 0xDB
    assert e.status == 0x01
    assert e.ext_status == 0x0100
    assert e.slot == 2


def test_cip_error_code_is_cip_status_sentinel():
    e = errors.BpCipError(service=0xDB, status=0x01, ext_status=0x0100, slot=2)
    assert e.code == errors.BP_ERR_CIP_STATUS == -400
    assert errors.err_code(e) == -400


def test_cip_status_message_known_pairs():
    # Connection in use (most common after a stale Forward_Open).
    assert "connection in use" in errors.cip_status_message(0x01, 0x0100).lower()
    # Transport class unsupported.
    assert "transport class" in errors.cip_status_message(0x01, 0x0103).lower()
    # Forward_Close conn-id-not-found.
    assert "connection id not found" in errors.cip_status_message(0x01, 0x0107).lower()
    # Status 0x02 = resource unavailable.
    assert "resource unavailable" in errors.cip_status_message(0x02, 0).lower()
    # Status 0x05 = path destination unknown.
    assert "path destination" in errors.cip_status_message(0x05, 0).lower()


def test_cip_status_message_unknown_pair_returns_safe_default():
    # Status 0x01 with no matching ext_status falls through to "connection failure".
    assert "connection failure" in errors.cip_status_message(0x01, 0xFFFF).lower()
    # Status 0xFF (made-up) hits the catch-all.
    assert "unknown" in errors.cip_status_message(0xFF, 0).lower()


def test_cip_error_message_includes_status_string():
    e = errors.BpCipError(service=0xDB, status=0x01, ext_status=0x0100, slot=2)
    s = str(e)
    assert "0xdb" in s.lower()
    assert "0x01" in s
    assert "0x0100" in s
    assert "slot=2" in s
    assert "connection in use" in s.lower()


def test_cip_error_is_bp_error_subclass():
    """BpCipError must inherit from BpError so callers can catch the
    base type and still get CIP-layer rejections."""
    e = errors.BpCipError(service=0xDB, status=0x01, ext_status=0x0100, slot=2)
    assert isinstance(e, errors.BpError)


def test_strerror_for_cip_status_code():
    s = errors.strerror(errors.BP_ERR_CIP_STATUS)
    assert "cip-layer rejection" in s.lower()
    assert "BpCipError" in s


if __name__ == "__main__":
    # Lightweight self-test for hosts without pytest installed.
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all CIP-error tests passed")
