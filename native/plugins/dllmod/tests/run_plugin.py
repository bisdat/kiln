#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""kiln-dllmod as a plugin under plain Wine, in compat's stand-in game (compathost.exe), no game:

    KILN_OUT=<dir> native/build.sh kiln-dllmod compathost
    KILN_OUT=<dir> python3 -I native/plugins/dllmod/tests/run_plugin.py [scenario...]

compathost is no Enshrouded build Kiln knows (its build info names exe c0ffee), so these scenarios check what the plugin
does before any hook: finding the profile's and the folder's packages, their statuses, the refusal on another build
and on a dedicated server, kiln/state/dllmod.json and kiln.log. The world start itself is tests/run_host.sh's.
"""
import json
import os
import shutil
import subprocess
import sys

OUT = os.path.abspath(os.environ.get("KILN_OUT", os.path.join(os.path.dirname(__file__), "../../../build")))
BIN = os.path.join(OUT, "cmake", "bin")
HOST = os.path.join(BIN, "compat-tests", "compathost.exe")
PLUGIN = os.path.join(BIN, "plugins", "kiln-dllmod.dll")
MODS = os.path.join(BIN, "dllmod-tests")
WORK = os.path.join(OUT, "dllmod-tests")
PFX = os.path.join(WORK, "pfx")
RUN = os.path.join(WORK, "plugin-run", "Игры (x86) 日本語")
WINE = os.environ.get("COMPAT_WINE", "wine")

PLAIN = "ENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=0.1.29-dev\r\nabi=1\r\n"
SETTINGS = "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=0.1.50-dev\nabi=1\nsetting=move_chests|bool|Chests|true\n"


def env():
    e = dict(os.environ)
    e.update(WINEPREFIX=PFX, WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d")
    e.pop("DISPLAY", None)
    e["WAYLAND_DISPLAY"] = "kiln-no-display"
    return e


class Game:
    def __init__(self, name, server=False):
        self.dir = os.path.join(RUN, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(os.path.join(self.dir, "kiln"))
        self.exe = "enshrouded_server.exe" if server else "enshrouded.exe"
        shutil.copy(HOST, self.p(self.exe))
        shutil.copy(PLUGIN, self.p("kiln-dllmod.dll"))
        self.failures = []

    def p(self, *parts):
        return os.path.join(self.dir, *parts)

    def package(self, folder, built, dll, manifest):
        os.makedirs(self.p(folder), exist_ok=True)
        if built:
            shutil.copy(os.path.join(MODS, built), self.p(folder, dll))
        if manifest is not None:
            with open(self.p(folder, dll + ".mod"), "w", encoding="utf-8", newline="") as f:
                f.write(manifest)

    def profile(self, entries, dropins=False):
        doc = {"format": 1, "id": "test", "name": "Test", "mods": {"dllmod": entries}}
        if dropins:
            doc["dropins"] = ["dllmod"]
        with open(self.p("kiln", "profile.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f)

    def run(self):
        r = subprocess.run([WINE, self.p(self.exe), "kiln-dllmod.dll", "300"], cwd=self.dir, env=env(),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        self.out = r.stdout.decode("utf-8", "replace")
        with open(self.p("kiln", "logs", "kiln.log"), encoding="utf-8", errors="replace") as f:
            self.log = f.read()
        try:
            with open(self.p("kiln", "state", "dllmod.json"), encoding="utf-8") as f:
                self.state = json.load(f)
        except OSError:
            self.state = {}
        return self

    def mod(self, mod_id):
        return next((m for m in self.state.get("mods", []) if m["id"] == mod_id), {})

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)


def unsupported():
    g = Game("unsupported")
    g.package("kiln/library/dllmod/lantern/v1", "dllmod_test_plain.dll", "lantern.dll", PLAIN)
    g.package("kiln/library/dllmod/carry/v1", "dllmod_test_settings.dll", "carry.dll", SETTINGS)
    g.package("kiln/library/dllmod/broken/v1", "dllmod_test_plain.dll", "broken.dll", "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=2\n")
    g.package("kiln/library/dllmod/empty/v1", "dllmod_test_plain.dll", "nothing.dll", None)
    g.profile([
        {"id": "lantern", "path": "library/dllmod/lantern/v1", "version": "0.1.29-dev"},
        {"id": "carry", "path": "library/dllmod/carry/v1", "enabled": False},
        {"id": "broken", "path": "library/dllmod/broken/v1"},
        {"id": "empty", "path": "library/dllmod/empty/v1"},
    ])
    g.run()
    s = g.state
    g.check(s.get("status") == "unsupported" and "26430625" in s.get("reason", ""), f"status unsupported: {s.get('status')} {s.get('reason')}")
    g.check(g.mod("lantern").get("status") == "skipped" and "5d9ef2f1" in g.mod("lantern").get("reason", ""), f"lantern: {g.mod('lantern')}")
    g.check(g.mod("lantern").get("package") == "v1/lantern.dll" and g.mod("lantern").get("version") == "0.1.29-dev", f"lantern ids: {g.mod('lantern')}")
    g.check(g.mod("carry").get("status") == "disabled", f"carry: {g.mod('carry')}")
    g.check(g.mod("broken").get("status") == "rejected" and "abi" in g.mod("broken").get("reason", ""), f"broken: {g.mod('broken')}")
    g.check(g.mod("empty").get("status") == "rejected" and "no DLL with a .dll.mod" in g.mod("empty").get("reason", ""), f"empty: {g.mod('empty')}")
    g.check("[W] [dllmod] mods for Enshrouded ModLoader are made for client build 26430625" in g.log, "kiln.log names the build")
    g.check("[W] [dllmod:broken] rejected:" in g.log, "kiln.log names the rejected mod")
    g.check("RESULT hooks_raw_links=0" in g.out, "nothing hooked")
    return g


def server():
    g = Game("server", server=True)
    g.package("kiln/library/dllmod/lantern/v1", "dllmod_test_plain.dll", "lantern.dll", PLAIN)
    g.profile([{"id": "lantern", "path": "library/dllmod/lantern/v1"}])
    g.run()
    g.check(g.state.get("status") == "client only" and g.state.get("side") == "server", f"server: {g.state.get('status')}")
    g.check(g.mod("lantern").get("status") == "skipped", f"lantern: {g.mod('lantern')}")
    return g


def dropins():
    g = Game("dropins")
    # no profile: the legacy layout reads Mods/EnshroudedModLoader (packages at any depth; a DLL without a manifest,
    # another loader's file and a library are no mods)
    g.package("Mods/EnshroudedModLoader/author/carryon", "dllmod_test_settings.dll", "CarryOn.dll", SETTINGS)
    g.package("Mods/EnshroudedModLoader/lantern", "dllmod_test_plain.dll", "lantern.dll", PLAIN)
    g.package("Mods/EnshroudedModLoader/lantern", "dllmod_test_lib.dll", "helper.dll", "ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1\nabi=1\n")
    g.package("Mods/EnshroudedModLoader/bare", "dllmod_test_plain.dll", "bare.dll", None)
    g.package("Mods/EnshroudedModLoader/other", "dllmod_test_plain.dll", "other.dll", "[settings]\n")
    g.run()
    ids = sorted(m["id"] for m in g.state.get("mods", []))
    g.check(ids == ["author/carryon/carryon.dll", "lantern/lantern.dll"], f"drop-ins found: {ids}")
    g.check(g.mod("author/carryon/carryon.dll").get("origin") == "folder", "origin folder")
    return g


def idle():
    g = Game("idle")
    g.profile([])
    g.run()
    g.check(g.state.get("status") == "idle" and g.state.get("mods") == [], f"idle: {g.state}")
    g.check("[W]" not in g.log, "no warning without mods")
    return g


SCENARIOS = {"unsupported": unsupported, "server": server, "dropins": dropins, "idle": idle}


def main():
    os.makedirs(WORK, exist_ok=True)
    if not os.path.isfile(os.path.join(PFX, "system.reg")):
        subprocess.run([WINE, "wineboot", "-i"], env=env(), stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=120)
    names = sys.argv[1:] or list(SCENARIOS)
    failed = 0
    for n in names:
        g = SCENARIOS[n]()
        if g.failures:
            failed += 1
            print(f"FAIL {n}:")
            for f in g.failures:
                print("   ", f)
            print(g.log)
        else:
            print(f"ok   {n}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
