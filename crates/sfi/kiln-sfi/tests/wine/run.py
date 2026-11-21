#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic headless Wine harness. Run python3 -I through the shared slot.
No game/corpus DLL is loaded. Outputs stay under --out; the private prefix is
--prefix (default <out>/prefix).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',required=True,type=Path)
    parser.add_argument('--xwin',type=Path,default=Path.home()/'.cache/cargo-xwin/xwin')
    parser.add_argument('--test-exe',type=Path,help='cargo-xwin --no-run wine_bindings test executable')
    parser.add_argument('--case',action='append',default=[],help='run only these named synthetic cases')
    parser.add_argument('--prefix',type=Path,help='private WINEPREFIX for every wine/wineserver call (default: <out>/prefix)')
    parser.add_argument('--prepare-only',action='store_true',help='build the fixtures and lay out every case folder (and probe/) for a native Windows run; start no Wine')
    args=parser.parse_args()
    source=Path(__file__).resolve().parent
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    include=[args.xwin/'crt/include',args.xwin/'sdk/include/ucrt',args.xwin/'sdk/include/um',args.xwin/'sdk/include/shared']
    libs=[args.xwin/'crt/lib/x86_64',args.xwin/'sdk/lib/ucrt/x86_64',args.xwin/'sdk/lib/um/x86_64']
    if not all(p.is_dir() for p in include+libs): raise RuntimeError('existing xwin SDK/CRT cache is incomplete; no downloads attempted')
    def build(name,src,defines=(),dll=False,imports=(),delay=False,delayload='helper.dll'):
        obj=out/(name+'.obj');obj.parent.mkdir(parents=True,exist_ok=True)
        command=['clang-cl','--target=x86_64-pc-windows-msvc','/nologo','/c','/MT','/GS-',*[arg for p in include for arg in ['-imsvc',str(p)]],*[f'/D{d}' for d in defines],str(source/src),f'/Fo{obj}']
        result=subprocess.run(command,cwd=out,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/(name+'.compile.log')).write_text(result.stdout)
        if result.returncode: raise RuntimeError(result.stdout)
        target=out/(name+('.dll' if dll else '.exe'))
        mode=(['/DLL',f'/IMPLIB:{target.with_suffix(".lib")}', '/ENTRY:_DllMainCRTStartup' if delay else '/ENTRY:DllMain',*([] if delay else ['/NODEFAULTLIB']),'kernel32.lib',*map(str,imports),*(['delayimp.lib',f'/DELAYLOAD:{delayload}'] if delay else [])] if dll else ['/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup'])
        result=subprocess.run(['lld-link','/nologo',*[f'/LIBPATH:{p}' for p in libs],str(obj),f'/OUT:{target}',*mode],cwd=out,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/(name+'.link.log')).write_text(result.stdout)
        if result.returncode:raise RuntimeError(result.stdout)
        return target
    a=build('mapped-a','fixture.c',['FIXTURE_VALUE=1'],True)
    b=build('mapped-b','fixture.c',['FIXTURE_VALUE=2'],True)
    host=build('mapping-probe','mapping_probe.c')
    prefix=args.prefix.resolve() if args.prefix else out/'prefix'
    home=out/'home';runtime=out/'runtime';home.mkdir(exist_ok=True);runtime.mkdir(mode=0o700,exist_ok=True)
    env=dict(os.environ,HOME=str(home),XDG_DATA_HOME=str(home/'data'),XDG_CONFIG_HOME=str(home/'config'),XDG_CACHE_HOME=str(home/'cache'),XDG_RUNTIME_DIR=str(runtime),WINEPREFIX=str(prefix),WINEARCH='win64',WINEDEBUG='-all',WAYLAND_DISPLAY='kiln-no-display',WINEDLLOVERRIDES='winedbg.exe=d;winemenubuilder.exe=d')
    env.pop('DISPLAY',None)
    def wine_path(p): return 'Z:'+str(p).replace('/','\\')
    loaded=out/'loaded.dll';(out/'renamed-a.dll').unlink(missing_ok=True);shutil.copyfile(a,loaded)
    if args.prepare_only:
        # A native runner calls probe/mapping-probe.exe loaded.dll mapped-b.dll renamed-a.dll from probe/.
        probe=out/'probe';probe.mkdir(exist_ok=True)
        for payload,name in [(host,'mapping-probe.exe'),(a,'loaded.dll'),(b,'mapped-b.dll')]:shutil.copyfile(payload,probe/name)
        result=subprocess.CompletedProcess([],0,'')
    else:
        command=['wine',str(host),wine_path(loaded),wine_path(b),wine_path(out/'renamed-a.dll')]
        result=subprocess.run(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
        print(result.stdout,end='')
    try:
        if result.returncode: raise RuntimeError(f'Wine mapping probe exit {result.returncode}')
        if args.test_exe:
            helper=build('good/helper','fixture.c',['FIXTURE_VALUE=1'],True)
            foreign=build('foreign/helper','fixture.c',['FIXTURE_VALUE=2'],True)
            entry=build('ordinary/entry','entry.c',dll=True,imports=[helper.with_suffix('.lib')])
            delayed=build('lazy/entry-delay','entry.c',dll=True,imports=[helper.with_suffix('.lib')],delay=True)
            fault=build('fault','fixture.c',['FIXTURE_FAULT=1'],True)
            fault_av=build('fault-av','fixture.c',['FIXTURE_AV=1'],True)
            fault_false=build('fault-false','fixture.c',['FIXTURE_FALSE=1'],True)
            fault_thread=build('fault-thread','fixture.c',['FIXTURE_THREAD_FAULT=1'],True)
            api_entry=build('api-entry','apiset_entry.c',dll=True,imports=['ucrt.lib'])
            # review of 9d5dead4: a delay dependency whose initializer fails, and the package that delay-imports it
            falsy=build('falsy/helper','fixture.c',['FIXTURE_FALSE=1'],True)
            falsy_entry=build('falsy/entry-delay','entry.c',dll=True,imports=[falsy.with_suffix('.lib')],delay=True)
            # O2: a package that delay-imports an OS DLL, and another loader's DLL of that name
            sysdelay=build('sysdelay','sysdelay_entry.c',dll=True,imports=['version.lib'],delay=True,delayload='version.dll')
            shadow=build('shadow/version','fixture.c',['FIXTURE_VALUE=3'],True)
            # a package that imports that OS DLL ordinarily (system-preloaded-collision)
            sysimport=build('sysimport','sysdelay_entry.c',dll=True,imports=['version.lib'])
            # O3 namespace-walk-stress: 16 distinct entries (an identical copy would rightly be the same DLL)
            walk_entries=[build(f'walk/entry-{k:02}','entry.c',[f'ENTRY_ID={k}'],dll=True,imports=[helper.with_suffix('.lib')]) for k in range(16)]
            touch=build('touch','fixture.c',['FIXTURE_TOUCH=1'],True)
            # Initializers that sleep past ShroudForge's 15 s startup deadline (19 s), or briefly (300 ms)
            sleep_long=build('sleep-long','fixture.c',['FIXTURE_SLEEP_MS=19000'],True)
            sleep_short=build('sleep-short','fixture.c',['FIXTURE_SLEEP_MS=300'],True)
            # review: 16 distinct packages that delay-import version.dll, loaded while other threads load and free it
            churn_entries=[build(f'churn/sysdelay-{k:02}','sysdelay_entry.c',[f'ENTRY_ID={k}'],dll=True,imports=['version.lib'],delay=True,delayload='version.dll') for k in range(16)]
            cases=['owner-tls','ordinary-correct','missing-dependency','preloaded-collision','unrelated-replaced','mapping-replaced','dllmain-fault','dllmain-av','dllmain-false','dllmain-thread-fault','system-api-set','lazy-correct','lazy-preloaded-collision','lazy-late-collision','lazy-after-close','diagnostic-burst','banned-burst','lazy-watch-failed-entry','renamed-identical-copy','preloaded-identical-helper','manager-order','manager-witness-change','lazy-unowned-shadow','lazy-system-shadow','system-preloaded-collision','unresolved-foreign','namespace-walk-stress','seh-inline-fault','foreign-failed-copy','midload-churn','unresolved-foreign-dependency','loader-respelled-name','lazy-owned-shadow','manager-order-never','manager-witness-change-never','manager-order-unverified','copies-over-capacity','boundary-never','boundary-unverified','boundary-not-wired','boundary-armed','deadline-hung','deadline-in-time']
            if any(case not in cases for case in args.case):raise ValueError('unknown synthetic case')
            cases=args.case or cases
            for case in cases:
                folder=out/'cases'/case;folder.mkdir(parents=True,exist_ok=True)
                executable=folder/'harness.exe';shutil.copyfile(args.test_exe,executable)
                shutil.copyfile(entry,folder/'entry.dll');shutil.copyfile(delayed,folder/'entry-delay.dll');shutil.copyfile(fault,folder/'fault.dll')
                for payload in [fault_av,fault_false,fault_thread,api_entry,sysdelay,sysimport]:shutil.copyfile(payload,folder/payload.name)
                shutil.copyfile(a,folder/'loaded.dll');shutil.copyfile(b,folder/'replacement.dll');shutil.copyfile(a,folder/'shroudforge-runtime.dll')
                (folder/'renamed.dll').unlink(missing_ok=True)
                if case!='missing-dependency':shutil.copyfile(helper,folder/'helper.dll')
                (folder/'foreign').mkdir(exist_ok=True);shutil.copyfile(foreign,folder/'foreign/helper.dll')
                for sub,files in [('falsy',[falsy,falsy_entry]),('b',[falsy]),('copy',[helper]),('shadow',[shadow])]:
                    (folder/sub).mkdir(exist_ok=True)
                    for payload in files:shutil.copyfile(payload,folder/sub/payload.name)
                shutil.copyfile(a,folder/'alias.dll');(folder/'moved-alias.dll').unlink(missing_ok=True)
                if case=='namespace-walk-stress':
                    # O3: 16 packages (distinct entry-NN.dll sharing one helper.dll file: a watch holds at most 8 alternative
                    # files per name) and 32 unrelated DLLs for churn threads (api-entry copies: headers unlike any package
                    # DLL's, so none is a candidate copy in first_loader)
                    (folder/'walk').mkdir(exist_ok=True);shutil.copyfile(helper,folder/'walk'/'helper.dll')
                    for k in range(16):shutil.copyfile(walk_entries[k],folder/'walk'/f'entry-{k:02}.dll')
                    (folder/'stress').mkdir(exist_ok=True)
                    for k in range(32):shutil.copyfile(api_entry,folder/'stress'/f's{k:02}.dll')
                if case=='copies-over-capacity':
                    # Nine packages, each with its own entry and its own identical helper.dll
                    for k in range(9):
                        (folder/'copies'/f'p{k}').mkdir(parents=True,exist_ok=True)
                        shutil.copyfile(walk_entries[k],folder/'copies'/f'p{k}'/f'entry-{k:02}.dll');shutil.copyfile(helper,folder/'copies'/f'p{k}'/'helper.dll')
                if case=='midload-churn':
                    (folder/'churn').mkdir(exist_ok=True)
                    for k in range(16):shutil.copyfile(churn_entries[k],folder/'churn'/f'sysdelay-{k:02}.dll')
                # A game folder with three packages for the native manager cases
                game=folder/'game'
                if game.exists():shutil.rmtree(game)
                for package,files in [('a',[(entry,'entry.dll'),(helper,'helper.dll'),(fault_false,'fault-false.dll'),(a,'third.dll')]),('b',[(b,'solo.dll')]),('c',[(a,'dep.dll')]),('t',[(touch,'touch.dll')]),('hl',[(sleep_long,'sleep.dll')]),('hs',[(sleep_short,'sleep.dll')]),('x',[(b,'x.dll')])]:
                    (game/'mods'/package).mkdir(parents=True)
                    for payload,name in files:shutil.copyfile(payload,game/'mods'/package/name)
                (game/'witness').mkdir();(game/'witness'/'input.bin').write_bytes(b'archive input as witnessed')
                if args.prepare_only:
                    continue
                case_env=dict(env,SFI_WINE_CASE=case,SFI_WINE_FIXTURES=wine_path(folder),SFI_TOUCH_FILE=wine_path(game/'witness'/'input.bin'))
                checked=subprocess.run(['wine',str(executable),'synthetic_wine_native_bindings','--exact','--nocapture','--test-threads=1'],env=case_env,cwd=folder,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
                (folder/'result.log').write_text(checked.stdout)
                if checked.returncode:raise RuntimeError(f'{case}: exit {checked.returncode}\n{checked.stdout}')
                print('PASS Wine',case)
        if args.prepare_only:
            (out/'cases.txt').write_text('\n'.join(cases)+'\n')
            print('prepared',len(cases),'cases in',out/'cases')
    finally:
        if not args.prepare_only:
            subprocess.run(['wineserver','-w'],env=env,timeout=120,check=True)


if __name__=='__main__': main()
