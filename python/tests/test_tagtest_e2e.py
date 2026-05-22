"""End-to-end tagtest round-trip.  Gated on BP_PLC_PATH so CI
doesn't need a live PLC.

Set BP_PLC_PATH=P:1,S:2 and BP_PLC_TAG=OCX_TEST (or override) to
exercise the full read/write/readback path.

SPDX-License-Identifier: MIT
"""
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import bpclient  # noqa: E402

PATH = os.environ.get("BP_PLC_PATH")
TAG = os.environ.get("BP_PLC_TAG", "OCX_TEST")


@pytest.mark.skipif(not PATH, reason="set BP_PLC_PATH (e.g. P:1,S:2) to run")
def test_tagtest_round_trip():
    with bpclient.Client() as c:
        c.open_session()
        db = c.open_tagdb(PATH)
        try:
            n = db.build()
            assert n > 0

            v0 = db.read_dint(TAG)
            sentinel = -559038737  # int32 of 0xDEADBEEF
            db.write_dint(TAG, sentinel)
            v1 = db.read_dint(TAG)
            assert v1 == sentinel

            db.write_dint(TAG, v0)
            v2 = db.read_dint(TAG)
            assert v2 == v0
        finally:
            db.close()
