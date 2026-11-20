#!/usr/bin/env bash
# remote-win1.sh samples SRC RWORK OUTDIRS: builds the sample mods of a release export on kiln-win1 (the owner's Windows
# PC, ssh kiln-win1) for tools/release/release.sh (KILN_BUILD_REMOTE=win1). Nothing signed and no key ever goes there:
# it gets the export (SRC, minus .cache and node_modules), the game-data catalog this machine's build would use (for
# Kiln.GameIds; none when there is none here), and sends back each sample's output folder, minus what the sample zips
# leave out anyway (the shared Kiln.* assemblies except Kiln.Admin.dll and Kiln.TestKit*, *.deps.json,
# *.runtimeconfig.json, assets/cooked/). release.sh zips them here exactly as it zips a local build's.
#   SRC      the release's export ($work/src, VERSION already the release's)
#   RWORK    a local folder for this job (emptied): up.tar, job.ps1, log; on success out/<rel> and the file ok
#   OUTDIRS  lines "<project>\t<local OutDir>" (lib/sample-outdirs.proj); the OutDirs are under $KILN_OUT/dotnet/
# Same compiler: the job uses the .NET SDK of the version this machine has (dotnet --version), installed on win1 at
# C:\kiln-runner\sdk\dotnet-<version> (dotnet-install.ps1 -Version <version> -InstallDir ...); a missing one fails the
# job (exit 3) and release.sh builds the samples here. PathMap maps the job's artifacts folder to this machine's
# $KILN_OUT/dotnet, so the DLLs name the same PDB path a local build does (lib/pe_equiv.py compares them).
# win1 is shared (someone games on it): the job takes a slot like /mnt/Dongus/kiln-wave/tools/win1 (KILN_WIN1_LOCKS,
# mode file "low": every slot, Windows Idle priority, 4 MSBuild nodes; "normal": one slot, Normal priority, 8 nodes).
# Env: KILN_OUT, KilnVersion, KILN_SDK_VERSION, KILN_GIT, SOURCE_DATE_EPOCH, KILN_RELEASE_BUILD, ContinuousIntegrationBuild
# (release.sh's) go to the build; KILN_REMOTE_HOST (default kiln-win1).
set -euo pipefail
[[ ${1:-} == samples && $# == 4 ]] || { echo "usage: $0 samples SRC RWORK OUTDIRS" >&2; exit 2; }
src=$(realpath -e -- "$2") rwork=$(realpath -m -- "$3") outdirs=$(realpath -e -- "$4")
host=${KILN_REMOTE_HOST:-kiln-win1}
: "${KILN_OUT:?}" "${KilnVersion:?}" "${SOURCE_DATE_EPOCH:?}"
t0=$(date +%s)
say() { printf 'remote-win1 (%ss): %s\n' $(($(date +%s) - t0)) "$*"; }
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 "$host" "$@" 2>&1 | grep -v -i -E 'post-quantum|store now, decrypt|openssh.com/pq|server may need to be upgraded'; return "${PIPESTATUS[0]}"; }
scp_() { scp -q -o BatchMode=yes -o ConnectTimeout=15 "$@" 2>&1 | grep -v -i -E 'post-quantum|store now, decrypt|openssh.com/pq|server may need to be upgraded'; return "${PIPESTATUS[0]}"; }
child=""
trap '[[ -n $child ]] && kill "$child" 2>/dev/null; exit 143' TERM INT HUP

rm -rf -- "$rwork" && mkdir -p "$rwork"
dotnet_dir=$(realpath -m -- "$KILN_OUT/dotnet")
ver=$(dotnet --version)
[[ $ver =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { say "unexpected local .NET SDK version '$ver'"; exit 2; }
job=r-$(printf '%s' "$rwork" | sha1sum | cut -c1-10)
J='C:\kiln-runner\release\'$job

# the projects and the output folders to send back (relative to the artifacts folder, forward slashes)
: >"$rwork/rels.txt"
printf '<Solution>\n' >"$rwork/samples.slnx"
while IFS=$'\t' read -r proj o; do
  [[ $proj == "$src/"* && $o == "$dotnet_dir/"* ]] || { say "unexpected project or output folder: $proj $o"; exit 2; }
  printf '  <Project Path="src/%s" />\n' "${proj#"$src/"}" >>"$rwork/samples.slnx"
  rel=${o#"$dotnet_dir/"}; printf '%s\n' "${rel%/}" >>"$rwork/rels.txt"
done <"$outdirs"
printf '</Solution>\n' >>"$rwork/samples.slnx"
n=$(wc -l <"$rwork/rels.txt")
((n > 0)) || { say "no sample projects"; exit 2; }

# the catalog this machine's build reads (managed/Kiln.Sdk/Sdk/Kiln.GameIds.targets, non-Windows order)
cat=""
if [[ -n ${KILN_CATALOG:-} ]]; then cat=$KILN_CATALOG
elif [[ -n ${KILN_DATA_DIR:-} ]]; then cat=$KILN_DATA_DIR/catalog
elif [[ -n ${XDG_DATA_HOME:-} ]]; then cat=$XDG_DATA_HOME/kiln/catalog
elif [[ -n ${HOME:-} ]]; then cat=$HOME/.local/share/kiln/catalog; fi
mkdir -p "$rwork/up/catalog"
remote_catalog=""
if [[ $cat == *.json && -f $cat ]]; then cp "$cat" "$rwork/up/catalog/"; remote_catalog="$J\\catalog\\$(basename "$cat")"
elif [[ -d $cat ]]; then
  for f in client.json server.json; do [[ -f $cat/$f ]] && cp "$cat/$f" "$rwork/up/catalog/"; done
  [[ -n $(ls -A "$rwork/up/catalog") ]] && remote_catalog="$J\\catalog"
fi
if [[ -n $remote_catalog ]]; then say "catalog: $cat"; else say "catalog: none"; fi

# the upload: src/ (the export), catalog/, samples.slnx, rels.txt
tar -cf "$rwork/up.tar" --exclude=./.cache --exclude=./launcher/node_modules --exclude=./tools/vscode/node_modules \
  -C "$src" --transform 's,^\.,src,' .
tar -rf "$rwork/up.tar" -C "$rwork/up" catalog -C "$rwork" samples.slnx rels.txt
say "upload: $(du -h "$rwork/up.tar" | cut -f1), $n sample projects, .NET SDK $ver"

# a slot on win1, like tools/win1
locks=${KILN_WIN1_LOCKS:-/mnt/Dongus/kiln-wave/tools/.win1}
mode=low nodes=4 prio=Idle
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
[[ $mode == normal ]] && { nodes=8; prio=Normal; }
say "win1 mode $mode: priority $prio, $nodes MSBuild nodes"

esc() { printf '%s' "$1" | sed "s/'/''/g"; }   # a PowerShell single-quoted string
cat >"$rwork/job.ps1" <<EOF
\$ErrorActionPreference = 'Continue'
(Get-Process -Id \$PID).PriorityClass = '$prio'
\$J = '$J'
\$D = 'C:\kiln-runner\sdk\dotnet-$ver'
if (-not (Test-Path "\$D\dotnet.exe")) { "remote-win1: no .NET SDK $ver at \$D (dotnet-install.ps1 -Version $ver -InstallDir \$D)"; exit 3 }
foreach (\$x in 'src','catalog','out','nodata','results.tar','samples.slnx','rels.txt','keep.txt','build.log') { if (Test-Path "\$J\\\$x") { Remove-Item -Recurse -Force "\$J\\\$x" } }
New-Item -ItemType Directory -Force "\$J\nodata" | Out-Null
\$t = Get-Date
tar -xf "\$J\up.tar" -C "\$J"
if (\$LASTEXITCODE -ne 0) { "remote-win1: extracting the upload failed"; exit 4 }
"remote-win1: extracted in " + [int]((Get-Date) - \$t).TotalSeconds + " s"
\$env:DOTNET_ROOT = \$D; \$env:DOTNET_MULTILEVEL_LOOKUP = '0'; \$env:Path = "\$D;" + \$env:Path
\$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'; \$env:DOTNET_NOLOGO = '1'; \$env:MSBUILDDISABLENODEREUSE = '1'
\$env:ContinuousIntegrationBuild = '$(esc "${ContinuousIntegrationBuild:-true}")'
\$env:KilnVersion = '$(esc "$KilnVersion")'
\$env:KILN_SDK_VERSION = '$(esc "${KILN_SDK_VERSION:-}")'
\$env:KILN_GIT = '$(esc "${KILN_GIT:-}")'
\$env:SOURCE_DATE_EPOCH = '$(esc "$SOURCE_DATE_EPOCH")'
\$env:KILN_RELEASE_BUILD = '$(esc "${KILN_RELEASE_BUILD:-}")'
\$env:KILN_OUT = "\$J\out"
\$env:NUGET_PACKAGES = 'C:\kiln-runner\release\nuget-packages'
\$env:KilnPython = 'python'
\$env:PathMap = "\$J\out\dotnet\=$(esc "$dotnet_dir")/"
\$env:KILN_DATA_DIR = "\$J\nodata"
Remove-Item Env:XDG_DATA_HOME -ErrorAction SilentlyContinue
if ('$(esc "$remote_catalog")' -ne '') { \$env:KILN_CATALOG = '$(esc "$remote_catalog")' } else { Remove-Item Env:KILN_CATALOG -ErrorAction SilentlyContinue }
# this release's own kiln.* packages never come from an earlier job's cache
Get-ChildItem "\$env:NUGET_PACKAGES\kiln.*\\$(esc "${KILN_SDK_VERSION:-none}")" -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
\$t = Get-Date
& "\$D\dotnet.exe" build "\$J\samples.slnx" -c Release --artifacts-path "\$J\out\dotnet" -v q -nologo -m:$nodes -nodeReuse:false *> "\$J\build.log"
\$rc = \$LASTEXITCODE
"remote-win1: dotnet build rc=\$rc in " + [int]((Get-Date) - \$t).TotalSeconds + " s"
Get-Content "\$J\build.log" -Tail 6
if (\$rc -ne 0) { Get-Content "\$J\build.log" | Select-String -Pattern ': error ' | Select-Object -First 30 | ForEach-Object { \$_.Line }; exit 5 }
# each output folder, minus what the zips leave out (release.sh's sample zip filter)
\$keep = New-Object System.Collections.Generic.List[string]
foreach (\$rel in Get-Content "\$J\rels.txt") {
  \$dir = Join-Path "\$J\out\dotnet" \$rel
  if (-not (Test-Path \$dir)) { "remote-win1: no output folder \$rel"; exit 6 }
  foreach (\$f in Get-ChildItem -LiteralPath \$dir -Recurse -File) {
    \$r = \$f.FullName.Substring((Resolve-Path -LiteralPath \$dir).Path.Length + 1).Replace('\', '/'); \$n = \$f.Name
    if ((\$n.StartsWith('Kiln.') -and \$n -ne 'Kiln.Admin.dll' -and -not \$n.StartsWith('Kiln.TestKit')) -or \$n.EndsWith('.deps.json') -or \$n.EndsWith('.runtimeconfig.json') -or \$r.StartsWith('assets/cooked/')) { continue }
    \$keep.Add(\$rel + '/' + \$r)
  }
}
[System.IO.File]::WriteAllLines("\$J\keep.txt", \$keep)
tar -cf "\$J\results.tar" -C "\$J\out\dotnet" -T "\$J\keep.txt"
if (\$LASTEXITCODE -ne 0) { "remote-win1: packing the results failed"; exit 7 }
"remote-win1: " + \$keep.Count + " files to send back"
exit 0
EOF

wpath=${J//\\//}
ssh_ "New-Item -ItemType Directory -Force '$J' | Out-Null" || { say "win1 does not answer"; exit 4; }
scp_ "$rwork/up.tar" "$rwork/job.ps1" "$host:$wpath/" || { say "upload failed"; exit 4; }
say "uploaded"
rc=0
ssh_ "powershell -NoProfile -ExecutionPolicy Bypass -File $J\\job.ps1" & child=$!
wait "$child" || rc=$?
child=""
((rc == 0)) || { say "the job failed (rc $rc)"; exit "$rc"; }
scp_ "$host:$wpath/results.tar" "$rwork/" || { say "download failed"; exit 4; }
mkdir -p "$rwork/out"
tar -xf "$rwork/results.tar" -C "$rwork/out"
rm -f -- "$rwork/up.tar" "$rwork/results.tar"
rm -rf -- "$rwork/up"
# The SDK's build tasks write the JSON they generate (kiln.mod.json, kiln.gametypes.json, impact/natives.json, ...) with
# the platform's line ends: CRLF on Windows, LF here. Back to LF, as a local build writes them, when no JSON file of the
# export has a CR (so every CR came from the Windows build); otherwise the job does not count.
if grep -rlIq --include='*.json' $'\r' "$src"; then say "the export has JSON files with CRLF: cannot tell generated line ends apart"; exit 8; fi
python3 -I - "$rwork/out" <<'PY'
import os, sys
n = 0
for d, _, files in os.walk(sys.argv[1]):
    for f in files:
        if f.endswith(".json"):
            p = os.path.join(d, f); b = open(p, "rb").read()
            if b"\r\n" in b:
                open(p, "wb").write(b.replace(b"\r\n", b"\n")); n += 1
print(f"remote-win1: {n} generated JSON files from CRLF to LF")
PY
while read -r rel; do [[ -d $rwork/out/$rel ]] || { say "no $rel in the results"; exit 6; }; done <"$rwork/rels.txt"
: >"$rwork/ok"
say "done: $n sample output folders in $rwork/out"
