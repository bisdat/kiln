# SPDX-License-Identifier: MIT
# Sourced by tools/release/release.sh (tests: tools/release/test_release_locks.py). Two helpers for a release run:
#
# release_lock DIR MESSAGE INFO: one release at a time per folder. Takes DIR/.release.lock (flock, the file descriptor
#   stays open for the rest of the calling shell's life, so the lock ends with the run, however it ends) and writes
#   INFO (who holds it) into the file. When another run holds it: prints "release: MESSAGE (its lock: FILE; ...)" with
#   that run's INFO and returns 1, having changed nothing. Processes the run starts inherit the descriptor, so a build
#   it left running keeps the folder locked too; when the run named in the file is gone, the message says so.
#   Why: two releases sharing the work folder or KILN_GLIBC_DIR swap each other's files (the 0.2.0-dev.30 T1b trial
#   shipped the kilnctl another run built minutes later in the same KILN_GLIBC_DIR/cargo).
#
# fresh_mtimes DIR EPOCH: every file, folder and symlink (the link itself) under DIR gets the time EPOCH. git archive
#   gives every exported file the commit's time, and Cargo (like make and Ninja) counts a crate as built when none of
#   its inputs is newer than its last build, so a reused target folder that last built a later commit would keep that
#   commit's code for an export of an older one. With EPOCH = now every input is newer than any earlier build: a full
#   rebuild of the workspace crates each run (dependencies from crates.io do not go by file times), slower but right.

release_lock() {
  local dir=$1 message=$2 info=$3 file fd held pid
  file=$dir/.release.lock
  mkdir -p -- "$dir" || return
  exec {fd}>>"$file" || { echo "release: cannot open the lock $file" >&2; return 1; }
  if ! flock -n "$fd"; then
    exec {fd}>&-
    held=$(head -c 512 -- "$file" 2>/dev/null | tr -d '\0' | head -1)
    pid=$(sed -n 's/^pid \([0-9][0-9]*\) .*/\1/p' <<<"$held")
    if [[ -n $pid ]] && ! kill -0 "$pid" 2>/dev/null; then
      held="$held; that run has ended, but a process it started still has the lock open (find it: fuser -v $file, or ls -l /proc/*/fd | grep release.lock)"
    fi
    echo "release: $message (its lock: $file; held by: ${held:-unknown})" >&2
    return 1
  fi
  # ours now: replace the last holder's line (writes through the O_APPEND descriptor land at the new end, 0)
  : >"$file"
  printf 'pid %s %s\n' "$$" "$info" >&"$fd"
}

fresh_mtimes() {
  local dir=$1 epoch=$2
  [[ -d $dir && $epoch =~ ^[0-9]+$ ]] || { echo "fresh_mtimes: needs a folder and a time (seconds), not '$dir' '$epoch'" >&2; return 2; }
  find "$dir" -exec touch -h -d "@$epoch" -- {} +
}
