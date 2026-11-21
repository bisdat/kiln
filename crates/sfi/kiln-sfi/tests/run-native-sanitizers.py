#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run through the shared slot; builds are separate and stay in --out."""
import argparse
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',required=True,type=Path)
a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True)
crate=Path(__file__).resolve().parents[1]
for name,flags in [('tsan',['-fsanitize=thread']),('asan-ubsan',['-fsanitize=address,undefined'])]:
    binary=a.out/('ring-'+name)
    command=['clang','-std=c11','-O1','-g','-pthread','-DSFI_RING_TEST_HOOKS',*flags,'-I',str(crate/'shim'),str(crate/'shim/notification_ring.c'),str(crate/'shim/breach_state.c'),str(crate/'shim/owner_faults.c'),str(crate/'shim/pin_state.c'),str(crate/'shim/seh_guard.c'),str(crate/'tests/native_ring_stress.c'),'-o',str(binary)]
    subprocess.run(command,check=True)
    env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1',ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for case in ['wrap','stress','burst-clean','burst-full-banned','burst-base-banned','pin-overlap','guard-decisions']:
        result=subprocess.run([str(binary),case],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
        (a.out/(name+'-'+case+'.log')).write_text(result.stdout)
        print(name,case,result.stdout,end='')
        if result.returncode:raise RuntimeError(f'{name}/{case} exit {result.returncode}')
    for target,sources,cases in [
        ('api-set',['api_set.c'],['version','empty']),
        ('load-window',['load_window.c'],['order']),
        ('owner-faults',['owner_faults.c','breach_state.c','notification_ring.c'],['isolation']),
        ('pe-identity',['pe_identity.c'],['headers']),
        ('copy-window',['copy_window.c'],['no-hold-close','bounds','race']),
    ]:
        output=a.out/(target+'-'+name)
        source_name={'api-set':'api_set','load-window':'load_window','owner-faults':'owner_faults','pe-identity':'pe_identity','copy-window':'copy_window'}[target]
        subprocess.run(['clang','-std=c11','-O1','-g','-pthread',*flags,'-I',str(crate/'shim'),*[str(crate/'shim'/s) for s in sources],str(crate/'tests'/(source_name+'_test.c')),'-o',str(output)],check=True)
        for case in cases:
            result=subprocess.run([str(output),case],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
            (a.out/(name+'-'+target+'-'+case+'.log')).write_text(result.stdout)
            print(name,target,case,result.stdout,end='')
            if result.returncode:raise RuntimeError(f'{name}/{target}/{case} exit {result.returncode}')
