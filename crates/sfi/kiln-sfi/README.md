<!-- SPDX-License-Identifier: MIT -->
# kiln-sfi

This MIT crate is Kiln's interpreter of ShroudForge mods: the plugin boundary
(`kiln_plugin_early` and `kiln_plugin_main` with Kiln's actual C ABI), the interpreter
worker on kiln.tasks, the lifecycle host that runs sfi-core's Lua runtime, the runtime
families that implement ShroudForge's `runtime` API on Kiln's services, the native
manager for package DLLs, and kiln-sfi's own state file (`kiln/state/sfi.json`).

It runs only when the core's frozen kiln.launch record chose backend `kiln` and the
held capability `kiln-sfi.interpreter` is lifted; every other start leaves it
inactive, and ShroudForge's own runtime stays the default. An inactive start returns
`KILN_OK` from both phases and reports `kiln-sfi.interpreter` unavailable/POLICY once
per phase owner, with its reason (for example `backend hosted` or `backend installed`).
One info line is logged across phases. The core's held-capability mechanism remains
the only client gate; this crate adds no config switch. Healthy inactivity requires a
usable `kiln.caps` prefix through a populated `report` slot. An absent/malformed
required table returns a contract error; a refused report returns an internal error,
not fake success. Owner reservation and successful reporting are tracked separately,
so identical re-entry can repair failed reporting without accepting a conflicting
identity. No host call runs under the boundary mutex.

## Boundaries

- Generated `kiln-abi` types come solely from MIT public headers, not EML's GPL
  partial Rust mirrors. Raw covered fields are copied without constructing a
  reference to a full newer table when a core supplies an older prefix.
- All exported Rust boundaries catch panics. Future host callbacks must catch
  their own panics too; no panic, C++ exception or Lua error may cross an ABI.
- `worker` runs on kiln.tasks: one LongRunning, RESIDENT task under kiln-sfi's own
  plugin owner. It marks its thread with `native_enforcement::mark_interpreter` on
  Windows (set on entry, cleared on exit), so a fault on it stops every mod runtime,
  not one package. No world/window/render callback waits for Lua.
- `shim/fault.c`, compiled by `cc`, directly calls typed mod init/native function
  pointers inside a Windows SEH frame. There is no intervening Rust trampoline.
  Host tests only exercise forwarding; they do not test Windows SEH recovery.
  Opaque pointer signatures are adapted from ShroudForge's MIT native header,
  1.8.0 `modules/mod-runtime/native_bridge/shroudforge_mod_native.h:79-81,133-136`.
- The protected DLL-load helper uses `LoadLibraryExW` with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, verified
  in 1.7.9 `modules/mod-runtime/native_dlls.rs:383-388` and 1.8.0 `:397-402`.
  DllMain can fault under OS loader lock. Admission/import scanning must precede
  the helper; a fault quarantines the package. Never FreeLibrary a mod module,
  including partial/faulted init. No loader or teardown path runs by default.
  Native hangs and arbitrary memory corruption are not made safe by SEH.

## Read-only import gate

`pe_gate`, `native_contracts` and `launch_plan` live in the pure MIT crate
`sfi-policy` so a host-side consumer such as kiln-manage can use them without
this plugin, `cc` or Lua; kiln-sfi re-exports them under the old paths. The
OS-bound pieces (`NativeContracts::evaluate_bound`, `pe_gate::parse_system`) sit
behind sfi-policy's `os-binding` feature, which only this crate enables. A host
inventory uses `NativeContracts::inventory`, whose `AdvisoryInventory` is facts,
never a load permit.

`pe_gate` parses bounded AMD64 PE32+ DLL snapshots, ordinary and delay imports
(RVA and legacy VA forms), ordinal/name thunks, and export forwarders. It follows
package-local dependencies with cycle tracking, rejects ambiguous/case-colliding
inventory, and reports banned module names regardless of case or path spelling.
The low-level `pe_gate` reports structural inspection separately from effective
admission. `native_contracts` applies the admission rules: malformed/opaque graphs and
static banned imports are refused; banned-name/prefix evidence without an exact
SHA-256 contract needs a contract; generic unknown dynamic behavior with no banned
name is admitted only with runtime enforcement. String evidence is never a proof
of a presence probe or of a guarded export path. Exact-hash reviewed presence
probes need explicit absent-path evidence; feature-disabled absence is partial and
needs a launcher warning. Guarded dynamic export resolution needs a supported
alternative. No player/plan flag creates a review. The versioned, host-owned
`sfi-native-contracts.json` (in `sfi-policy`, embedded as `NativeContracts::shipped()`)
ships empty; its schema is alongside it. No real-mod
contracts are supplied. Every admitted verdict still requires actual initial and
post-load module checks plus process-lifetime loader notification enforcement. Non-ASCII inventory keys are conservatively
unknown until an actual Windows identity/search binding owns their normalization.

`sfi-pe-inspect <directory>...` is a read-only corpus diagnostic with no trusted
reviews or system-module approvals by default. It never loads a DLL. A pass of
this pure inspector is not an admission token: the later loader must verify exact
opened-file identity, authorized dependency search and actual system/loaded-image
ownership immediately before load. No gate is wired into plugin activation yet.

## Native runtime enforcement primitives

`native_enforcement` binds private permits to a closed ordinary/lazy graph of
package snapshots and real OS/API-set bindings, including demanded forwarders.
Unknown imports refuse executable admission; the CLI's advisory verdict cannot
manufacture system-binding proof. Package dependencies must physically share the
entry DLL's folder (as SF's load flags require). Windows file handles deny writes
and deletes; canonical ancestors deny deletion, and legitimate remaining mount/
reparse anchors also deny write sharing. Game/Steam-library junctions are not
blanket-rejected. Identity, size and SHA-256 are checked again before use.

Mapped identity is proved against a read-only `SEC_IMAGE_NO_EXECUTE` view made from
the retained handle, using `NtAreMappedFilesTheSame`. A reopened loader pathname is
only a candidate hint; it is never proof. Missing/unsupported backing proof refuses
loading. A CAS-owned observation window publishes its bound names/backing views
before the pre-scan. Short OS loader-lock holds protect metadata scans/publication
only: **no LoadLibrary call runs while SFI holds LdrLockLoaderLock**, avoiding the
Windows parallel loader's load-owner/loader-lock inversion. Ordinary verified OS
images are preloaded outside that lock; normal LoadLibraryExW initializes the
package closure. Notifications match name + backing, never a presumed loader TID,
so Windows loader workers and interleaving foreign loads remain observable. The
entry notification also rechecks active bound names using read-only loader metadata.
It never eagerly preloads a package dependency. Package delay targets remain lazy
and mandatory proofs. Trusted OS DLLs' own delay contracts are not recursively
expanded (optional empty-host ext-ms sets are valid absence); API-set v6 lookup
matches HashedLength's versioned prefix, with importer-specific/default hosts.

Accepted detection-only limit: a replacement discovered at LOADED can run DllMain
before quarantine. There is no EntryPoint/undocumented loader-field write and no
global interception. The offending package is pinned/quarantined, never retried;
sticky banned-runtime proof still detects a swapped DLL's forbidden dependency.
DLL side effects are not rolled back. This residual is a parity difference, not a
claim of pre-entry prevention; native-Windows timing/coexistence needs end checks.
`ForeignOwned` never initializes again and is not proof of required SF host bridge
registrations. First-loader binding checks cover dependencies, not only the entry.

The plugin's Windows early entry performs a read-only module audit before any
future admission. A pre-existing banned runtime is reported with the hosted
self-hosting reason and remains inactive, not quarantined-and-started. An armed
native run (made in the early phase, the policy allows, the plan lists a DLL
and the startup witness is wired) constructs `Enforcer::early`
on its one loader thread (`kiln-sfi native`), audits again, registers
`LdrRegisterDllNotification` through a C SEH boundary before its first package DLL,
and closes the scan/register race. A run that is not armed registers nothing and
starts no thread. An armed run whose terminal goes to kiln-eml first starts a
deadline waiter (`kiln-sfi native deadline`), and the loader thread does nothing
until it runs (a thread started while a DllMain holds the loader lock could not run):
at ShroudForge's 15 s, counted from the arming, the waiter pushes the terminal with
the unfinished DLLs timed out while the loader thread stays inside the initializer,
never stopped; the run has one terminal, pushed once. The notification and pinned callback code live
until process exit. The loader-lock callback only compares/copies bounded POD
through lock-free atomics: no Rust/Lua, allocation, logging, waiting or unloading.
Notifications are drained outside loader lock. Banned-name proof is a sticky
process-lifetime atomic bitmap set from BOTH base and full final-component names
before diagnostic publication. Ring loss alone never proves a breach: a full
loader-locked module scan recovers state, with quarantine only on confirmed proof
or an actual scan/observer failure. A banned load/unload cannot disappear inside an
overrun. Every actual load also gets a post-load audit; there is no unload/fallback.

The callback pins every matching permitted package image with
`LdrAddRefDll(LDR_ADDREF_DLL_PIN)` before DllMain. Pin/LOADED evidence and backing
identity remain process-lifetime C records. Failed loads after LOADED, pin failures,
SEH faults and after-entry errors (998/1114/exception codes) are `NativeFault`, not
ordinary missing-file refusals. Failed pinned images remain quarantined and may
never be returned as `ForeignOwned` or retried; successful pins prevent loader
rollback from unmapping code DllMain published. Pin failure cannot promise that
protection. A clean loader-converted1114/998 failure with a successful pin stops
only its owning package; independent packages remain eligible. Known owner faults
have their own process-lifetime atomic catalog, independent of diagnostic loss.
Banned runtimes, unowned breaches, stack overflow, pin failure or SEH through our
own frames remain global invariants. Foreign-copy candidates are prefiltered on
mapped PE timestamp/image size/checksum/section headers before opening a pathname;
unverifiable unrelated overlays are ignored, never claimed as foreign ownership.
An arbitrary other loader's reuse of an already mapped pinned image
may produce no notification; observing every such request would need cooperation
or a separate shared loader interception, not implemented here.

Debugger breakpoint/single-step exceptions continue search. Stack overflow is
handled only after unwinding: `_resetstkoflw` restores the guard before returning
`NativeFault`. If restoration fails, a C atomic/event signal wakes a healthy
supervisor, then the damaged thread parks non-alertably for process life without
returning to Rust/Lua/mod code. Interpreter-worker failure stops all mod runtimes;
a known non-worker package thread stops its package (unknown ownership is global).
The supervisor sends the lifecycle/quarantine/record/save warning independently of
the parked worker. This rare failure does not fail-fast the player's game. Loader
callbacks let stack overflow unwind to the outer guard so parking cannot retain
our transaction's loader lock. Explicit worker role marks avoid guessing from a
thread name. No active runtime is started by these primitives.

A Sink receives quarantine, durable-record and not-to-save requests outside loader
lock; the interpreter wires them to the package runtime, launch state and UI.
Fake-backend tests verify refusal, first-loader behavior,
identity/hash changes and these sink calls. `tests/run-native-sanitizers.py --out
<private-output>` builds actual shared C ring/breach/pin-state/guard-decision code
separately with TSan and ASan/UBSan. `tests/wine/run.py --out <private-output>
--test-exe <cargo-xwin --no-run wine_bindings executable>` builds synthetic own
DLLs and runs the actual Windows backend in a private headless prefix. Both scripts
must run through the project's build limiter; no game/corpus DLL is executed. The Wine runner
isolates HOME/XDG, disables debuggers/menu builders and drains its wineserver.

Native-Windows cases that Wine and compilation do not establish (they need a real Windows game):

| Native-Windows case | Required evidence |
| --- | --- |
| Rust-built mod DLL on native Windows | Real System32/API-set version aliases admit without recursively expanding optional OS delays; system backing mismatch branches work (Wine builtins alias their placeholders). |
| Concurrent Windows loads | A slow foreign DLL load overlaps the package load without load-owner/loader-lock deadlock; notifications on loader workers are matched by identity. |
| Detection-only replacement residual | Measure LOADED/dependency/entry DllMain order; mismatched image may initialize but is pinned, quarantined, not retried; no EntryPoint mutation. |
| DllMain RaiseException/access violation/FALSE/thread-then-fault | Both shim-SEH and loader-converted paths quarantine, warn, retain backing identity, never retry; game survives. |
| `LdrAddRefDll(PIN)` inside LOADED under loader lock | No deadlock/allocation dependency; successful and failed package images remain mapped. |
| Failed load of a pinned image | First-loader query returns quarantined, never foreign-success/re-init; later known-loader attempts are named in status. |
| Junctioned/symlinked Steam/game library | Legitimate installation admits; mapped-A/replaced-path-B still refuses and unrelated file IO remains possible. |
| Real stack overflow and `_resetstkoflw` | Guard restoration precedes Rust return; injected restoration failure parks the thread, healthy supervisor warns, no game fail-fast. |
| Managed quarantine/save-warning integration | Actual runtime withdrawal and acknowledged UI/state delivery, with archive presence remaining truthful. |

Native hangs/arbitrary memory corruption are not made transactional or sandboxed
by SEH/pinning. The policy/PE verdict cache and bounded boot scanning are a follow-up
boundary change, keyed by SHA-256 plus PE timestamp/SizeOfImage; no cache grant may
replace current opened-file/backing/namespace proof.

## Frozen launch-plan reader

`launch_plan` reads at most 8 MiB/depth 64 and validates v1 with the plan's
root `startupDialect`, canonical UUID plan id and null unstamped launch id. It
rejects unknown/duplicate keys, noncanonical integers, wrong executable hash/side,
unsafe Windows-relative paths, case collisions, inconsistent parts/DLL admissions,
missing or misordered included dependency closure, writer/backend contradictions,
and 1.8 candidates without independent trusted release approval. It retains the
whole validated document read-only. SemVer resolution belongs to sfi-core's
planner; this reader validates the recorded closure and does not re-solve ranges.

`permission` yields only `EligiblePendingAdmission` when selected backend is kiln
and the caller's held-capability check allows it. Hosted/installed/external/none
stay inactive. This is not a startup seal, filesystem/content fingerprint proof,
service-readiness proof, or runtime activation. The real core still must validate
current profile/package/chain/receipt identities at use, load admitted DLLs under
enforcement, compile Lua/create environments and complete the startup barrier.
Plugin exports remain inactive; the reader never starts anything.

## Build and integration

Run `build.sh` through the project's build limiter, setting `KILN_OUT` and
`CARGO_TARGET_DIR`. It uses cached `cargo xwin`, the MIT workspace's own toolchain
and lock, static CRT, panic=unwind, and preserves encoded path-remapping flags.
Before workspace integration, `SFI_MANIFEST` may point to a throwaway workspace
copy; never alter the existing repository workspace or create an independent
Rust version pin. `CARGO_BUILD_BUILD_DIR` can redirect a global Cargo build-dir
setting into the assigned output area too.

The staged file is `$KILN_OUT/cmake/bin/plugins/kiln-sfi.dll`. Packaging needs its
own MIT component and licence notices; **never** place it in the GPL bundled
`kiln/plugins/shroudforge/` component; `tools/package/stage.sh` stages it as its own
MIT component.
