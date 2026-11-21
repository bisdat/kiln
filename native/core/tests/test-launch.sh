#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.launch on the host (no Windows, no game): src/launch.cpp as it is, three builds and two source checks:
#   launch_test             the validator's decision table, the idle class, the held row, the frozen record's
#                           members, the ShroudForge-dialect owner, launch.json's text and its once-per-boot write;
#                           ASan+UBSan
#   launch_test (TSan)      the same, built with ThreadSanitizer: readers racing the freeze, designations racing
#   launch_boot_order_test  boot.cpp's boot() up to the signatures step, cut out of the file as it is, against stubs:
#                           the freeze comes after compat's early rules and before native mods and early plugins, which
#                           see kiln.launch frozen, with boot's own build_verified answer (0 for backend none)
#   load_one                api.cpp's plugin loader (Windows code): the designation is asked for plugins only, for
#                           kiln/plugins/kiln-sfi.dll, with the build-stamp test, before the plugin's kiln_plugin_main
#   launch_boot             launch_boot.cpp (Windows code): the sf18 veto is the release policy's alone
#                           (sfi_release_policy.h), the hold is the client gate's alone (no "preview" read here),
#                           nothing escapes freeze_at_boot, and the record's native_policy is kiln.json native.policy
#                           as config() read it, for every decision, and its build_verified is boot's verified, handed in
#   native/core/tests/test-launch.sh
# Environment: KILN_OUT (output folder, default native/build), KILN_TEST_REPEAT (runs of each binary, default 1),
# KILN_LAUNCH_SRC / KILN_BOOT_SRC / KILN_API_SRC / KILN_LAUNCH_BOOT_SRC: another launch.cpp / boot.cpp / api.cpp /
# launch_boot.cpp (to see a broken rule fail).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
out=${KILN_OUT:-$here/../../build}/launch-test
mkdir -p "$out"
launch_src=${KILN_LAUNCH_SRC:-$src/launch.cpp}
boot_src=${KILN_BOOT_SRC:-$src/boot.cpp}
api_src=${KILN_API_SRC:-$src/api.cpp}
launch_boot_src=${KILN_LAUNCH_BOOT_SRC:-$src/launch_boot.cpp}
repeat=${KILN_TEST_REPEAT:-1}
cxx=${CXX:-clang++}
common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I"$src" -I"$here/../abi" -I"$here/../../third_party")

python3 -I - "$boot_src" "$out" <<'PY'
import pathlib, sys
boot = pathlib.Path(sys.argv[1]).read_text()
start = boot.index('void boot() {')
end = boot.index('    progress_state::step("signatures");', start)
(pathlib.Path(sys.argv[2]) / 'launch_boot_order.inc').write_text(boot[start:end] + '}\n')
PY

# load_one's designation, read from the source as it is: one block, gated on kind "plugin", the frozen backend, the
# exact file and the build stamp, and before the call of the plugin's kiln_plugin_main.
python3 -I - "$api_src" <<'PY'
import pathlib, re, sys
api = pathlib.Path(sys.argv[1]).read_text()
start = api.index('void load_one(const std::filesystem::path& file, const char* kind) {')
body = api[start:api.index('\n}\n', start)]
fails = []
gate = re.search(r'if \(std::string_view\(kind\) == "plugin" && launch::wants_dialect_owner\(\) && '
                 r'launch::is_dialect_dll\(file, paths\(\)\.plugins\)\) \{', body)
call = body.find('launch::designate(owner, service_stamp_trusted(dll_build_stamp(h)), why)')
main = body.find('guard::plugin_main(')
if not gate: fails.append('the designation is not gated on kind "plugin", the frozen backend and kiln/plugins/kiln-sfi.dll')
if call < 0: fails.append('the designation does not pass the build-stamp test of the DLL itself')
if body.count('launch::designate(') != 1: fails.append('load_one must ask for the designation exactly once')
if main < 0 or (gate and gate.start() > main) or call > main:
    fails.append("the designation must come before the plugin's kiln_plugin_main")
if gate and body.find('reg::owner_create(', 0, gate.start()) < 0:
    fails.append('the designation must come after the owner is created (it designates that owner)')
for f in fails: print('FAIL load_one: ' + f, file=sys.stderr)
print(f'load_one: 5 checks, {len(fails)} failures')
sys.exit(1 if fails else 0)
PY

# launch_boot.cpp (Windows code, no host build), read from the source as it is: the kiln-sfi.sf18 veto is the release
# policy's (sfi_release_policy::release_allows_180(), closed in every release and preview build), the interpreter's hold
# is the client gate's after kiln.json "preview" (gates::configure applies it; nothing here reads "preview"), and nothing
# escapes freeze_at_boot (kiln_boot's catch would end Kiln's whole boot).
python3 -I - "$launch_boot_src" <<'PY'
import pathlib, re, sys
src = pathlib.Path(sys.argv[1]).read_text()
code = re.sub(r'//[^\n]*', '', src)  # comments say "release_allows_180" too
fails, checked = [], []
def need(ok, what):
    checked.append(what)
    if not ok: fails.append(what)
need(re.findall(r'release_allows_180[^;]*;', code) == ['release_allows_180 = sfi_release_policy::release_allows_180();'],
     'the kiln-sfi.sf18 veto must come from the release policy alone: in.release_allows_180 = '
     'sfi_release_policy::release_allows_180()')
need(code.count('interpreter_held') == 1 and 'in.interpreter_held = hold.held;' in code,
     'interpreter_held must come from interpreter_hold() alone')
need('const Hold hold = interpreter_hold(gate_held, gate_text);' in code,
     'the hold must be asked with the client gate alone')
need('preview' not in code and 'previewed' not in code,
     'kiln.json "preview" must not be read here: the client gate applies it (gates::configure), the release policy after it')
need('bool gate_held = false;' in code and code.count('gate_held = true') == 1 and
     'if (h.name == kInterpreterGate) gate_held = true, gate_text = h.text;' in code,
     'the gate hold must come from gates::held() alone')
start = code.find('void freeze_at_boot(')
body = code[start:] if start >= 0 else ''
need(re.search(r'try \{\s*d = decide_at_boot\(side, safe_idle\);\s*\} catch \(const std::exception& e\) \{\s*'
               r'd = undecided\(e\.what\(\)\);\s*\} catch \(\.\.\.\) \{\s*d = undecided\(', body) is not None,
     'freeze_at_boot must catch everything decide_at_boot throws and freeze undecided()')
need(re.search(r'try \{\s*took = freeze\(d, id, stamp, write_state\);\s*\} catch', body) is not None,
     'freeze_at_boot must catch what freeze() throws')
need(re.search(r'const auto write_state = \[&\] \{\s*try \{', body) is not None,
     "launch.json's write must catch what it throws")
need(re.search(r'std::random_device[^}]*\}\s*catch \(\.\.\.\)', code, re.S) is not None,
     "the launch id must not throw when std::random_device does")
# native_policy is config().native_policy (the value native mods follow), each value by name, never by a default
# that allows; and it is set for whatever the decision was (undecided() included), after it and before the freeze.
need(re.search(r'uint32_t frozen_native_policy\(\) \{\s*switch \(config\(\)\.native_policy\) \{\s*'
               r'case NativePolicy::Always: return KILN_LAUNCH_NATIVE_POLICY_ALWAYS;\s*'
               r'case NativePolicy::VerifiedOnly: return KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY;\s*'
               r'case NativePolicy::Never: return KILN_LAUNCH_NATIVE_POLICY_NEVER;\s*\}\s*'
               r'return KILN_LAUNCH_NATIVE_POLICY_NEVER;\s*\}', code) is not None,
     "native_policy must map config().native_policy case by case (anything else: never)")
need(len(re.findall(r'native_policy\s*=(?!=)', code)) == 1 and
     re.search(r'catch \(\.\.\.\) \{\s*d = undecided\("an unknown error"\);\s*\}\s*'
               r'd\.native_policy = frozen_native_policy\(\);', body) is not None and
     body.find('d.native_policy = frozen_native_policy();') < body.find('took = freeze('),
     "freeze_at_boot must set d.native_policy once, right after the decision (undecided too), before the freeze")
# LP1b: build_verified is the verified boot.cpp hands in (its own build_verified answer), set once for every decision,
# right after native_policy and before the freeze; freeze() keeps it 0 for backend none (launch_test).
need('void freeze_at_boot(uint32_t side, bool verified, const std::string& safe_idle) {' in code,
     'freeze_at_boot must take boot\'s verified')
need(len(re.findall(r'build_verified\s*=(?!=)', code)) == 1 and
     re.search(r'd\.native_policy = frozen_native_policy\(\);\s*d\.build_verified = verified \? 1u : 0u;', body) is not None,
     "freeze_at_boot must set d.build_verified from boot's verified once, right after native_policy, before the freeze")
for f in fails: print('FAIL launch_boot: ' + f, file=sys.stderr)
print(f'launch_boot: {len(checked)} checks, {len(fails)} failures')
sys.exit(1 if fails else 0)
PY

asan=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
"$cxx" "${common[@]}" "${asan[@]}" "$here/launch_test.cpp" "$launch_src" -o "$out/launch_test"
"$cxx" "${common[@]}" -fsanitize=thread -DKILN_TEST_TSAN "$here/launch_test.cpp" "$launch_src" -o "$out/launch_test_tsan"
"$cxx" "${common[@]}" "${asan[@]}" -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -I"$out" "$here/launch_boot_order_test.cpp" \
  "$launch_src" -o "$out/launch_boot_order_test"

for ((i = 1; i <= repeat; i++)); do
  "$out/launch_test"
  TSAN_OPTIONS=halt_on_error=1 "$out/launch_test_tsan"
  "$out/launch_boot_order_test"
done
