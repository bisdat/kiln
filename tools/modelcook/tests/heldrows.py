# SPDX-License-Identifier: MIT
"""Hold cook feature rows for one test: the switch cook/features.py keeps for a row the owner holds at a release. Every row is on
since 0.2.0-dev.31; the refusals of a held row are still tested through this.

    with held("modelcook.cloth"):
        ...   # modelcook.cloth is off by default, with the reason "game test pending: <its checks>"
"""
import contextlib
import dataclasses
from unittest import mock

from cook import features as F


@contextlib.contextmanager
def held(*names):
    """Rows `names` (all rows when none is named) off by default for the duration."""
    names = set(names or F.BY_NAME)
    unknown = names - set(F.BY_NAME)
    if unknown:
        raise KeyError(f"not cook feature rows: {sorted(unknown)}")
    rows = tuple(dataclasses.replace(f, default=False) if f.name in names else f for f in F.ROWS)
    with mock.patch.object(F, "ROWS", rows), mock.patch.object(F, "BY_NAME", {f.name: f for f in rows}):
        yield
