#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage only the pinned EMF Lua host and its licenses, never EMF's loader or scripts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

RELEASE = "0.6.22"
LUA = "1.43.0"
ARCHIVE_NAME = f"EMF-{RELEASE}.zip"
UPSTREAM = "https://github.com/matenm/emf-enshrouded"
URL = f"{UPSTREAM}/releases/download/v{RELEASE}/{ARCHIVE_NAME}"
ARCHIVE_SHA256 = "fb25c4fdb8f65929e0908587de5a8d62c580623ff9f6e59428896a4d63d3a76f"
MEMBERS = {
    "lua.dll": f"EMF-{RELEASE}/game/emf/mods/lua/lua.dll",
    "mod.json": f"EMF-{RELEASE}/game/emf/mods/lua/mod.json",
    "EMF-LICENSE.txt": f"EMF-{RELEASE}/LICENSE.txt",
    "Lua-LICENSE.txt": f"EMF-{RELEASE}/licenses/Lua-LICENSE.txt",
}
SOURCE = (f"EMF's unmodified Lua host {LUA}, from EMF {RELEASE} (MIT).\n"
          f"Artifact: {URL}\nArchive SHA-256: {ARCHIVE_SHA256}\n"
          f"Source: {UPSTREAM}/tree/v{RELEASE}/mods/lua and src/lua_host.\n")


def verified_files(archive):
    with archive.open("rb") as file:
        digest = hashlib.sha256()
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
        if digest.hexdigest() != ARCHIVE_SHA256:
            raise ValueError(f"{archive}: not the pinned EMF {RELEASE} archive (SHA-256 {digest.hexdigest()})")
        file.seek(0)
        with zipfile.ZipFile(file) as z:
            # Select exact members and assign their names ourselves; do not unpack an archive tree.
            files = {name: z.read(member) for name, member in MEMBERS.items()}
    manifest = json.loads(files["mod.json"])
    if (manifest.get("id"), manifest.get("version"), manifest.get("emf")) != ("lua", LUA, 20) or \
            manifest.get("main") is not None or manifest.get("dll", "lua.dll") != "lua.dll":
        raise ValueError(f"{archive}: unexpected Lua mod manifest")
    return {**files, "SOURCE.txt": SOURCE.encode("utf-8")}


def fetch(cache):
    archive = cache / ARCHIVE_NAME
    if archive.exists():
        return verified_files(archive)
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fetch-", dir=cache) as tmp:
        downloaded = Path(tmp) / ARCHIVE_NAME
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3",
                        "--proto", "=https", "--output", str(downloaded), URL], check=True)
        files = verified_files(downloaded)
        os.replace(downloaded, archive)
        return files


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("destination", type=Path)
    ap.add_argument("--archive", type=Path, default=os.environ.get("KILN_EMF_LUA_ARCHIVE"),
                    help="use a local copy of the exact pinned archive (same SHA-256 check)")
    args = ap.parse_args()
    try:
        cache = Path(__file__).resolve().parents[2] / ".cache" / "emf-lua"
        files = verified_files(args.archive) if args.archive else fetch(cache)
        if args.destination.is_symlink() or any((args.destination / name).is_symlink() for name in files):
            raise ValueError("destination contains a symbolic link")
        args.destination.mkdir(parents=True, exist_ok=True)
        for name, data in files.items():
            (args.destination / name).write_bytes(data)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as e:
        ap.exit(1, f"stage-emf-lua: {e}\n")


if __name__ == "__main__":
    main()
