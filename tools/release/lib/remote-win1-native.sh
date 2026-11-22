#!/usr/bin/env bash
# remote-win1-native.sh SRC RWORK: builds the release's native Windows DLLs (the CMake targets: kiln-core, the dinput8
# proxy, kiln-native-hello, every native service and plugin but kiln-shroudlink, which is Rust and builds on this
# machine) on kiln-win1 for tools/release/release.sh (KILN_BUILD_REMOTE=win1), with the SAME toolchain as here: the
# same clang-cl/lld-link/llvm-rc release (LLVM for Windows from the llvm-project GitHub release, at
# C:\kiln-runner\sdk\llvm-<version>) and this machine's xwin MSVC CRT + Windows SDK (sent once, kept by content hash in
# C:\kiln-runner\sdk\xwin-<hash>), through native/cmake/clang-cl-xwin.cmake, with release.sh's path maps applied to
# win1's paths. So the DLLs are this machine's but for the PDB id (8 bytes; lib/pe_equiv.py). Nothing signed goes
# there: the DLLs come back unsigned and release.sh signs them here as before.
#   SRC    the release's export ($work/src, VERSION already the release's)
#   RWORK  a local folder for this job (emptied): on success bin/ (as $KILN_OUT/cmake/bin: *.dll *.pdb, services/,
#          plugins/, and kiln_build.h: the build's stamp, its generated/kiln_build.h, which tools/package/stage.sh builds
#          kiln-sfi.dll with) and the file ok
# A different clang version on win1, or no LLVM there, fails the job (exit 3): release.sh then builds them here.
# Env: SOURCE_DATE_EPOCH, KILN_GIT, KILN_RELEASE_BUILD (release.sh's), RELEASE_WORK (release.sh's $work: the path maps),
# XWIN_ROOT (default ~/.cache/cargo-xwin/xwin), KILN_REMOTE_HOST (default kiln-win1); win1's slots as remote-win1.sh.
set -euo pipefail
[[ $# == 2 ]] || { echo "usage: $0 SRC RWORK" >&2; exit 2; }
src=$(realpath -e -- "$1") rwork=$(realpath -m -- "$2")
host=${KILN_REMOTE_HOST:-kiln-win1}
: "${SOURCE_DATE_EPOCH:?}" "${RELEASE_WORK:?}"
t0=$(date +%s)
say() { printf 'remote-win1-native (%ss): %s\n' $(($(date +%s) - t0)) "$*"; }
quiet_ssh() { grep -v -i -E 'post-quantum|store now, decrypt|openssh.com/pq|server may need to be upgraded'; }
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 "$host" "$@" 2>&1 | quiet_ssh; return "${PIPESTATUS[0]}"; }
scp_() { scp -q -o BatchMode=yes -o ConnectTimeout=15 "$@" 2>&1 | quiet_ssh; return "${PIPESTATUS[0]}"; }
child=""
trap '[[ -n $child ]] && kill "$child" 2>/dev/null; exit 143' TERM INT HUP
rm -rf -- "$rwork" && mkdir -p "$rwork"
work=$(realpath -m -- "$RELEASE_WORK")

# the toolchain here: clang's version, the xwin splat and its content hash
llvm=$(clang-cl --version | sed -n '1s/.*clang version \([0-9][0-9.]*\).*/\1/p')
[[ $llvm =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { say "no clang-cl version here"; exit 2; }
xwin=$(realpath -e -- "${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}")
(cd "$xwin" && find crt sdk -type f -not -path '*/aarch64/*' -not -path '*/arm64/*' | LC_ALL=C sort) >"$rwork/xwin.list"
xhash=$(cd "$xwin" && tr '\n' '\0' <"$rwork/xwin.list" | xargs -0 sha256sum | sha256sum | cut -c1-16)
# the xwin root as the build here names it after the path maps (release.sh: $work, then $HOME, then $src)
case $xwin in
  "$work"/*) xmapped=/build/work${xwin#"$work"} ;;
  "$HOME"/*) xmapped=/build${xwin#"$HOME"} ;;
  *) xmapped=$xwin ;;
esac
targets=(kiln-core kiln-proxy kiln-native-hello)
for dir in "$src"/native/services/*/ "$src"/native/plugins/*/; do
  [[ -f $dir/CMakeLists.txt ]] || continue
  t=kiln-$(basename "$dir"); [[ $t == kiln-shroudlink ]] && continue
  targets+=("$t")
done
say "clang $llvm, xwin $xhash ($(wc -l <"$rwork/xwin.list") files), ${#targets[@]} targets"

tar -cf "$rwork/up.tar" --exclude=./.cache --exclude=./launcher/node_modules --exclude=./tools/vscode/node_modules --exclude=./third_party \
  -C "$src" --transform 's,^\.,src,' .
say "upload: $(du -h "$rwork/up.tar" | cut -f1)"

# a slot on win1, like tools/win1 and remote-win1.sh
locks=${KILN_WIN1_LOCKS:-/mnt/Dongus/kiln-wave/tools/.win1}
mode=low prio=Idle jobs=8
if [[ -d $locks ]]; then
  mode=$(cat "$locks/mode" 2>/dev/null || echo low)
  slots=$(cat "$locks/slots" 2>/dev/null || echo 2); [[ $slots =~ ^[0-9]+$ && $slots -ge 1 ]] || slots=2
  if [[ $mode == normal ]]; then
    got=""
    while [[ -z $got ]]; do
      for i in $(seq 1 "$slots"); do exec {fd}>>"$locks/$i.lock"; if flock -n "$fd"; then got=$i; break; fi; exec {fd}>&-; done
      [[ -n $got ]] || sleep 5
    done
  else
    for i in $(seq 1 "$slots"); do exec {fd}>>"$locks/$i.lock"; flock "$fd"; done
  fi
fi
[[ $mode == normal ]] && { prio=Normal; jobs=16; }
say "win1 mode $mode: priority $prio, $jobs jobs"

job=n-$(printf '%s' "$rwork" | sha1sum | cut -c1-10)
J='C:\kiln-runner\release\'$job
X='C:\kiln-runner\sdk\xwin-'$xhash
L='C:\kiln-runner\sdk\llvm-'$llvm
esc() { printf '%s' "$1" | sed "s/'/''/g"; }
cat >"$rwork/job.ps1" <<EOF
\$ErrorActionPreference = 'Continue'
(Get-Process -Id \$PID).PriorityClass = '$prio'
\$J = '$J'; \$X = '$X'; \$L = '$L'
if (-not (Test-Path "\$L\bin\clang-cl.exe")) { "remote-win1-native: no LLVM $llvm at \$L"; exit 3 }
\$v = (& "\$L\bin\clang-cl.exe" --version | Select-Object -First 1)
if (\$v -notmatch '^clang version $llvm( |\$)') { "remote-win1-native: \$L is '\$v', not clang $llvm"; exit 3 }
if (-not (Test-Path "\$X\crt\include")) { "remote-win1-native: no xwin \$X"; exit 9 }
foreach (\$old in 'src','out','results.tar','configure.log','build.log','keep.txt') { if (Test-Path "\$J\\\$old") { Remove-Item -Recurse -Force "\$J\\\$old" } }
\$t = Get-Date
tar -xf "\$J\up.tar" -C "\$J"
if (\$LASTEXITCODE -ne 0) { "remote-win1-native: extracting the upload failed"; exit 4 }
\$vs = & "\${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
\$cm = "\$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
\$env:Path = "\$L\bin;\$cm\CMake\bin;\$cm\Ninja;C:\Program Files\Git\bin;" + \$env:Path
foreach (\$n in 'INCLUDE','LIB','LIBPATH','VCToolsInstallDir','VCINSTALLDIR','WindowsSdkDir','CFLAGS','CXXFLAGS','LDFLAGS') { Remove-Item "Env:\\\$n" -ErrorAction SilentlyContinue }
\$env:XWIN_ROOT = \$X
\$env:SOURCE_DATE_EPOCH = '$(esc "$SOURCE_DATE_EPOCH")'; \$env:KILN_GIT = '$(esc "${KILN_GIT:-}")'; \$env:KILN_RELEASE_BUILD = '$(esc "${KILN_RELEASE_BUILD:-}")'
\$env:KILN_OUT = "\$J\out"
# release.sh's path maps, for win1's paths: the work folder, the home folder, the export; the xwin root as it is named here
\$pm = '/clang:-ffile-prefix-map="' + \$J + '=/build/work" /clang:-ffile-prefix-map="' + \$env:USERPROFILE + '=/build" /clang:-ffile-prefix-map="' + \$J + '\src=/kiln" /clang:-ffile-prefix-map="' + \$X + '=$(esc "$xmapped")"'
\$env:CFLAGS = \$pm; \$env:CXXFLAGS = \$pm; \$env:LDFLAGS = '/pdbaltpath:%_PDB%'
cmake -S "\$J\src\native" -B "\$J\out\cmake" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=\$J\src\native\cmake\clang-cl-xwin.cmake" -DCMAKE_BUILD_TYPE=Release -Wno-author -Wno-deprecated *> "\$J\configure.log"
if (\$LASTEXITCODE -ne 0) { Get-Content "\$J\configure.log" -Tail 20; exit 5 }
cmake --build "\$J\out\cmake" -j $jobs --target ${targets[*]} *> "\$J\build.log"
\$rc = \$LASTEXITCODE
"remote-win1-native: build rc=\$rc, " + [int]((Get-Date) - \$t).TotalSeconds + " s with the upload's extraction"
if (\$rc -ne 0) { Get-Content "\$J\build.log" | Select-String -Pattern 'error|FAILED' | Select-Object -First 30 | ForEach-Object { \$_.Line }; exit 5 }
\$b = "\$J\out\cmake\bin"
\$keep = New-Object System.Collections.Generic.List[string]
Get-ChildItem \$b -File | Where-Object { \$_.Extension -in '.dll','.pdb' } | ForEach-Object { \$keep.Add(\$_.Name) }
# this build's stamp (generated/kiln_build.h): the stage builds kiln-sfi.dll with it, so it carries the core's stamp
Copy-Item "\$J\out\cmake\generated\kiln_build.h" "\$b\kiln_build.h" -ErrorAction SilentlyContinue
if (-not (Test-Path "\$b\kiln_build.h")) { "remote-win1-native: the build wrote no generated/kiln_build.h"; exit 7 }
\$keep.Add('kiln_build.h')
foreach (\$d in 'services','plugins') { if (Test-Path "\$b\\\$d") { Get-ChildItem "\$b\\\$d" -File -Recurse | ForEach-Object { \$keep.Add(\$d + '/' + \$_.FullName.Substring((Resolve-Path "\$b\\\$d").Path.Length + 1).Replace('\', '/')) } } }
[System.IO.File]::WriteAllLines("\$J\keep.txt", \$keep)
tar -cf "\$J\results.tar" -C \$b -T "\$J\keep.txt"
if (\$LASTEXITCODE -ne 0) { "remote-win1-native: packing the results failed"; exit 7 }
"remote-win1-native: " + \$keep.Count + " files to send back"
exit 0
EOF
cat >"$rwork/xwin.ps1" <<EOF
\$ErrorActionPreference = 'Continue'
\$X = '$X'
if (Test-Path "\$X\crt\include") { 'present'; exit 0 }
if (Test-Path "\$X.tmp") { Remove-Item -Recurse -Force "\$X.tmp" }
New-Item -ItemType Directory -Force "\$X.tmp" | Out-Null
tar -xf 'C:\kiln-runner\sdk\xwin-$xhash.tar' -C "\$X.tmp"
if (\$LASTEXITCODE -ne 0) { 'extracting failed'; exit 4 }
Move-Item "\$X.tmp" \$X
Remove-Item 'C:\kiln-runner\sdk\xwin-$xhash.tar'
'installed'
EOF

wj=${J//\\//}
ssh_ "New-Item -ItemType Directory -Force '$J','C:\\kiln-runner\\sdk' | Out-Null" || { say "win1 does not answer"; exit 4; }
# the xwin splat, once per content hash
scp_ "$rwork/xwin.ps1" "$host:$wj/" || { say "upload failed"; exit 4; }
if ! ssh_ "powershell -NoProfile -ExecutionPolicy Bypass -File $J\\xwin.ps1" | grep -q present; then
  say "sending the xwin splat $xhash"
  (cd "$xwin" && tar -cf "$rwork/xwin.tar" -T "$rwork/xwin.list")
  scp_ "$rwork/xwin.tar" "$host:C:/kiln-runner/sdk/xwin-$xhash.tar" || { say "upload failed"; exit 4; }
  rm -f -- "$rwork/xwin.tar"
  ssh_ "powershell -NoProfile -ExecutionPolicy Bypass -File $J\\xwin.ps1" | grep -q installed || { say "installing the xwin splat failed"; exit 4; }
fi
scp_ "$rwork/up.tar" "$rwork/job.ps1" "$host:$wj/" || { say "upload failed"; exit 4; }
say "uploaded"
rc=0
ssh_ "powershell -NoProfile -ExecutionPolicy Bypass -File $J\\job.ps1" & child=$!
wait "$child" || rc=$?
child=""
((rc == 0)) || { say "the job failed (rc $rc)"; exit "$rc"; }
scp_ "$host:$wj/results.tar" "$rwork/" || { say "download failed"; exit 4; }
mkdir -p "$rwork/bin"
tar -xf "$rwork/results.tar" -C "$rwork/bin"
rm -f -- "$rwork/up.tar" "$rwork/results.tar"
[[ -s $rwork/bin/kiln-core.dll && -s $rwork/bin/dinput8.dll ]] || { say "no kiln-core.dll/dinput8.dll in the results"; exit 6; }
grep -q '^#define KILN_BUILD "+' "$rwork/bin/kiln_build.h" 2>/dev/null || { say "no build stamp (kiln_build.h) in the results"; exit 6; }
: >"$rwork/ok"
say "done: $(find "$rwork/bin" -name '*.dll' | wc -l) DLLs in $rwork/bin"
