-- SPDX-License-Identifier: MIT
T.note("before failure", "still recorded")
error("fixture failed")
T.note("unreachable", "must not run")
