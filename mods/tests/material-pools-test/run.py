#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Coordinator-only lab runner. Importing this module or running test_run.py performs no lab/network operation.
Use two NEW disposable labs; --boundary-lab isolates ID exhaustion from the main world's retained materials.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[2]
PROOF=['capabilities','vanilla-rows-unchanged',*[kind+'-'+key for key in ('four','shared','full','roof') for kind in ('membership','binding','preview-data','budget')],
       'shared-material-one-id','no-65-served','vanilla-shape-binding','material-budgets','unknown-pool']
SERVED=['served-registration','served-idempotent','65-refused-before-serving','duplicate-refused','roof-refused','unknown-base-refused']
FINAL=['served','restart','absent','back','late-material-room','handshake-order-before-stream','handshake-binding-before-stream','late','pending-inactive','replay','torn-refused',
       'stored-pool-dropped','stored-pool-gone','restored-main-hash','observer-cost','building-seed','building-retained','terrain-boundary']

def valid_result(mode,result,log,required):
    if result.get('mode')!=mode or result.get('failed')!=0 or not re.fullmatch('[0-9a-f]{16}',str(result.get('hash',''))):return False
    passes=result.get('passed',[])
    return all(n in passes and 'mp: PASS '+n in log for n in required) and 'mp: FAIL ' not in log and f"mp: DONE {mode} {len(passes)} passed 0 failed hash {result['hash']}" in log

def rejected_before_stream(welcome,reject,data_frames,names=None):
    # names: what the refusal must say (the material pool that differs), when the peer sent its pools
    return welcome is None and reject is not None and 'building' in reject[0].lower() and 'materials' in reject[0].lower() and data_frames==0 and (names is None or names in reject[0])

def pool_digest(guid,roof,items,shapes):
    """Kiln.Network's WorldTablesGate.PoolDigest: FNV-1a 64 over what a pool gives a player's menu."""
    h=0xcbf29ce484222325
    def add(text):
        nonlocal h
        for b in (text+'\n').encode():h=((h^b)*0x100000001b3)&0xffffffffffffffff
    add(guid);add('roof' if roof else 'wall')
    for i in items:add(str(i))
    add('shapes')
    for shape in sorted(shapes):add(shape)
    return f'{h:016x}'

def pools_property(status):
    bound=[(b['pool'],b['item']) for b in status.get('shapeBindings',[])]
    return ','.join(f"{p['name']}={pool_digest(p['guid'],p['roof'],p['items'],[i for (q,i) in bound if q==p['guid']])}" for p in status.get('customPools',[]))

class Lab:
    def __init__(self,name,build,out,port,position,base_box):
        if not re.fullmatch('[a-zA-Z0-9._-]+',name) or name in ('.','..'):raise ValueError('invalid lab name')
        self.name=name;self.root=Path(os.environ.get('KILN_LABS',Path.home()/'srv/enshrouded/labs'))/name
        if not (self.root/'enshrouded_server.exe').is_file():raise ValueError('create a new disposable lab first: '+str(self.root))
        self.kiln=self.root/'kiln';self.build=build;self.out=out;self.port=port;self.position=position
        self.data=self.kiln/'data/material-pools-test';self.data.mkdir(parents=True,exist_ok=True)
        (self.data/'base-box.json').write_text(json.dumps(base_box))
        if position is not None:(self.data/'position.json').write_text(json.dumps(position))
        else:(self.data/'position.json').unlink(missing_ok=True)
        self.stop()
        unknown=[p.name for p in (self.kiln/'mods').glob('*') if p.is_dir() and not p.name.startswith('mp-') and p.name!='material-pools-test']
        if unknown:raise ValueError('lab must contain only this test: '+str(unknown))
        # Refuse reused lab state: the two labs are intentionally isolated. Do not delete anybody's world ledger.
        if list((self.kiln/'data/worldtables/worlds').glob('*.json')):raise ValueError('use a NEW lab with no prior worldtables ledger')
        d=self.kiln/'mods/material-pools-test';d.mkdir(parents=True,exist_ok=True)
        shutil.copy(build/'dotnet/bin/MaterialPoolsTest/debug/MaterialPoolsTest.dll',d)
        shutil.copy(HERE/'kiln.mod.json',d)
        self.authored_driver(True)
        config=self.kiln/'config/network.json';config.parent.mkdir(parents=True,exist_ok=True)
        j=json.loads(config.read_text()) if config.exists() else {};j['labPort']=port;config.write_text(json.dumps(j))
    def tool(self,verb):
        command=['bash',str(REPO/'tools/lab/lab.sh'),verb,self.name]
        # MP_LABSTART: the build wave's lab starter (its shared limit on running labs and its memory scope), called as
        # `<starter> <lab name>` from the repository root, in place of lab.sh start.
        if verb=='start' and os.environ.get('MP_LABSTART'):command=[os.environ['MP_LABSTART'],self.name]
        elif verb=='start':command=['bash','-c','ulimit -e 1; exec nice -n 19 ionice -c 3 "$@"','mp-lab',*command]
        return subprocess.run(command,check=True,capture_output=True,text=True,cwd=str(REPO)).stdout
    def stop(self):self.tool('stop')
    def authored_driver(self,present):
        dst=self.kiln/'mods/material-pools-test/blocks';shutil.rmtree(dst,ignore_errors=True)
        if present:shutil.copytree(HERE/'blocks',dst)
    def fixture(self,document=None):
        dst=self.kiln/'mods/mp-fixture';shutil.rmtree(dst,ignore_errors=True)
        shutil.copytree(HERE/'fixtures/mp-fixture',dst)
        if document is not None:(dst/'blocks/pools.json').write_text(json.dumps(document))
    def remove(self,name):
        if not name.startswith('mp-'):raise ValueError('only this test owns mp-*')
        shutil.rmtree(self.kiln/'mods'/name,ignore_errors=True)
    def content(self,name,document):
        self.remove(name);dst=self.kiln/'mods'/name;(dst/'blocks').mkdir(parents=True)
        (dst/'kiln.mod.json').write_text(json.dumps(dict(id=name,name=name,version='1.0.0',apiVersion='0.2.0-dev.29',side='both',clients='mod')))
        (dst/'blocks/data.json').write_text(json.dumps(document))
    def run(self,name,mode,required,during=None):
        (self.data/'mode').write_text(mode);(self.data/'result.json').unlink(missing_ok=True);(self.data/'served.json').unlink(missing_ok=True)
        log=self.kiln/'logs/kiln.log';log.unlink(missing_ok=True)
        self.out.mkdir(parents=True,exist_ok=True)
        try:
            self.tool('start');end=time.monotonic()+480
            while time.monotonic()<end:
                text=log.read_text(errors='replace') if log.exists() else ''
                if f'mp: DONE {mode} ' in text and (self.data/'result.json').is_file():break
                time.sleep(1)
            else:raise RuntimeError(name+': missing fresh completion/result')
            result=json.loads((self.data/'result.json').read_text());status=json.loads((self.data/'served.json').read_text())
            if not valid_result(mode,result,text,required) or result['hash']!=status['hash']:raise RuntimeError(name+': failed/missing actual lab observations')
            if during:during(status)
            return result,status
        finally:
            self.stop()
            if log.exists():shutil.copy(log,self.out/(name+'.log'))
            for source in ('result.json','served.json','cells.json'):
                if (self.data/source).exists():shutil.copy(self.data/source,self.out/(name+'-'+source))

def handshake(lab,status,old_hash=None,old_pools=None,names=None):
    # Existing Kiln loopback protocol. Reject must precede WELCOME and ANY DATA/FRAG, including partial streams.
    sys.path.insert(0,str(REPO/'tools/nettest'))
    from nettest import Client,T_DATA,T_FRAG
    mods=[]
    for p in sorted((lab.kiln/'mods').glob('*/kiln.mod.json')):
        j=json.loads(p.read_text());mods.append((j['id'],j['version'],2))
    ids=','.join(f"{m['id']}={uuid.UUID(m['guid']).hex}" for m in status['materials'] if m['state']=='live')
    def one(table_hash,pools):
        c=Client(lab.port,'material-pools-'+uuid.uuid4().hex[:8])
        try:
            props={'kiln.worldtables':table_hash,'kiln.worldtables.ids':ids}
            if pools:props['kiln.worldtables.pools']=pools
            c.hello(mods,[],props)
            c.wait(lambda:c.welcome is not None or c.reject is not None,10);c.pump(.5)
            return c.welcome,c.reject,int(T_DATA in c.frame_len)+int(T_FRAG in c.frame_len)
        finally:c.s.close()
    welcome,reject,_=one(status['hash'],pools_property(status))
    if welcome is None or reject is not None or welcome['props'].get('kiln.worldtables')!=status['hash']:raise RuntimeError('matching profile was not welcomed with its actual table hash')
    if old_hash is not None:
        if old_hash==status['hash']:raise RuntimeError('changed pool/binding did not change the actual service hash')
        w,r,frames=one(old_hash,old_pools)
        if not rejected_before_stream(w,r,frames,names):raise RuntimeError('mismatching profile was not refused before blocks streamed, naming '+str(names)+': '+str(r))
        print('  refused: '+r[0],flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--lab',required=True);ap.add_argument('--boundary-lab',required=True)
    ap.add_argument('--port',type=int,default=28351);ap.add_argument('--boundary-port',type=int,default=28355)
    ap.add_argument('--position',nargs=3,type=float,metavar=('X','Y','Z'),help='optional clear-ground override inside the base in both save copies')
    ap.add_argument('--base-box',nargs=6,type=float,default=[3006,902.6,1530,3042,906.6,1570],metavar=('X0','Y0','Z0','X1','Y1','Z1'),help='base bounds and vertical clear-cell search range in metres')
    ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
    if a.lab==a.boundary_lab:raise ValueError('the boundary run requires its own new lab')
    build=Path(os.environ['KILN_OUT']);passed=[]
    lab=Lab(a.lab,build,a.out/'main',a.port,a.position,a.base_box);boundary=Lab(a.boundary_lab,build,a.out/'boundary',a.boundary_port,a.position,a.base_box)
    def done(name):passed.append(name);print('PASS mp-lab.'+name,flush=True)
    try:
        fixture=json.loads((HERE/'fixtures/mp-fixture/blocks/pools.json').read_text());lab.fixture()
        initial,status=lab.run('served','served',PROOF+SERVED+['all-ground-read-before-write','cells-written-and-game-saved','restart-materials-and-cells-live'],during=lambda s:handshake(lab,s));done('served')
        again,_=lab.run('restart','restart',PROOF+['restart-materials-and-cells-live'])
        if initial['hash']!=again['hash']:raise RuntimeError('hash changed on identical restart')
        done('restart')
        lab.remove('mp-fixture');lab.run('absent','absent',['absent-materials-and-cells-retained']);done('absent')
        lab.fixture();lab.run('back','restart',PROOF+['restart-materials-and-cells-live']);done('back')
        lab.run('late-material','late-material',['late-material-uses-vanilla-room']);done('late-material-room')
        staged=lab.kiln/'data/worldtables/pending/material-pools-test/ungrouped.json'
        if 'ungrouped' not in json.loads(staged.read_text()).get('buildingMaterials',{}):raise RuntimeError('missing actual staged ungrouped material')
        staged.unlink()  # only this deliberately transient lab definition; keep the main profile unchanged
        # A client of the first profile (its hash and per-pool digests) joins a changed one: refused before any stream, by pool name.
        old_pools=pools_property(status)
        changed=json.loads(json.dumps(fixture));changed['buildingMaterialPools']['four']['materials'].reverse();lab.fixture(changed)
        lab.run('order','status',['status-snapshot'],during=lambda s:handshake(lab,s,initial['hash'],old_pools,'the material pool mp-fixture:four differs'));done('handshake-order-before-stream')
        changed=json.loads(json.dumps(fixture));changed['blueprints']['four_shape']['materialPool']='shared';lab.fixture(changed)
        lab.run('binding','status',['status-snapshot'],during=lambda s:handshake(lab,s,initial['hash'],old_pools,'the material pool mp-fixture:shared differs'));done('handshake-binding-before-stream')
        # No boot pool at all: exercises the production catalog's first-late-call path.
        lab.remove('mp-fixture');lab.authored_driver(False)
        late,_=lab.run('late','late',['late-registry-closed','late-accepted-zero-slots','late-idempotent','late-not-live','explicit-remove']);done('late')
        envelopes=list((lab.kiln/'data/worldtables/pending/material-pools-test').glob('@pool-*.json'))
        if len(envelopes)!=1:raise RuntimeError('expected ONE durable definition+receipt envelope')
        envelope=envelopes[0];valid=json.loads(envelope.read_text())
        if valid.get('mod')!='material-pools-test' or valid.get('block')!='late':raise RuntimeError('wrong durable envelope')
        # A different loaded observer must see a disabled owner's file refused once, unchanged and unapplied.
        original_data=lab.data;disabled=lab.kiln/'mods/material-pools-test/.disabled'
        observer=lab.kiln/'mods/mp-pending-observer';observer.mkdir(parents=True,exist_ok=True)
        manifest=json.loads((HERE/'kiln.mod.json').read_text());manifest['id']='mp-pending-observer'
        (observer/'kiln.mod.json').write_text(json.dumps(manifest))
        shutil.copy(build/'dotnet/bin/MaterialPoolsTest/debug/MaterialPoolsTest.dll',observer)
        lab.data=lab.kiln/'data/mp-pending-observer';lab.data.mkdir(parents=True,exist_ok=True)
        shutil.copy(original_data/'receipt.txt',lab.data/'receipt.txt');before=envelope.read_bytes();disabled.touch()
        try:
            lab.run('inactive','inactive',['inactive-request-unapplied','inactive-file-named'])
            if envelope.read_bytes()!=before:raise RuntimeError('inactive request changed or disappeared')
            lines=(lab.out/'inactive.log').read_text(errors='replace').splitlines()
            service=[line for line in lines if '[service:kiln-worldtables.dll]' in line and envelope.name in line]  # the service's own lines, not a mod's echo
            named=[line for line in service if 'its mod is not loaded' in line]
            if any('unsafe' in line for line in service):raise RuntimeError('a disabled mod\'s stored pool was called unsafe')
            if len(named)!=1:raise RuntimeError('inactive file must be named exactly once')
            done('pending-inactive')
        finally:
            disabled.unlink(missing_ok=True);lab.data=original_data;lab.remove('mp-pending-observer')
        lab.run('replay','replay',['restart-receipt-registered','restart-owner-adopted-before-redefine','restart-same-receipt','restart-one-row']);done('replay')
        # Torn file: neither a row nor a Registered receipt may survive a new process.
        valid_bytes=envelope.read_bytes()
        envelope.write_text('{"format":1,"canonical":');lab.run('torn','torn',['torn-envelope-refused']);done('torn-refused')
        # The stored pool is the mod's standing request only while the mod asks for it: loaded and no longer defining it, the
        # row is served this session, the file goes, and the next start has no row and the hash it had without the pool.
        envelope.write_bytes(valid_bytes)
        lab.run('drop','drop',['drop-row-served-this-session','drop-world-ran'])
        if envelope.exists():raise RuntimeError('the stored pool the mod no longer defines was not pruned: '+str(envelope))
        done('stored-pool-dropped')
        gone,_=lab.run('dropped','dropped',['dropped-row-gone'])
        if gone['hash']!=late['hash']:raise RuntimeError(f"hash after the stored pool went is {gone['hash']}, not the profile's own {late['hash']}")
        done('stored-pool-gone')
        lab.fixture();lab.authored_driver(True)
        restored,_=lab.run('restored','restart',PROOF+['restart-materials-and-cells-live'])
        if restored['hash']!=initial['hash']:raise RuntimeError('restored main profile changed its original hash')
        done('restored-main-hash')
        # What the pool observer costs a tick with the fixture's pools loaded (the driver measures and logs it).
        lab.run('observer-cost','observer-cost',['observer-quiet-when-unchanged']);done('observer-cost')
        # Seed 49 building IDs through roofs (50 selector places) so the 49-ID limit is independent of selector room.
        # Then remove the mod: all 49 retained records have NO selectable block items. A further live ID is refused.
        boundary.content('mp-cap-building',{'format':1,'buildingMaterials':{f'm{i}':{'name':f'ID seed {i}','roof':True} for i in range(49)}})
        boundary.run('building-seed','building-seed',['building-49-plus-1','boundary-refusal-count','all-ground-read-before-write','cells-written-and-game-saved']);done('building-seed')
        boundary.remove('mp-cap-building');boundary.content('mp-overflow',{'format':1,'buildingMaterials':{'overflow':{'name':'50th building ID','roof':True}}})
        boundary.run('building-retained','building-retained',['building-49-plus-1','boundary-refusal-count','retained-building-no-selector-items','absent-materials-and-cells-retained']);done('building-retained')
        boundary.remove('mp-overflow');boundary.content('mp-cap-terrain',{'format':1,'terrainMaterials':{f'm{i}':{'name':f'Terrain ID {i}','like':'TerrainStone','placeable':False} for i in range(12)}})
        boundary.run('terrain','terrain-boundary',['terrain-11-plus-1','boundary-refusal-count','terrain-no-selector-work']);done('terrain-boundary')
        if passed!=FINAL:raise RuntimeError('not every required lab case ran')
        proof={'format':1,'hash':initial['hash'],'kiln':initial.get('kiln'),'passed':passed,'complete':True}
        a.out.mkdir(parents=True,exist_ok=True);(a.out/'lab-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
        print('PASS mp-lab.ALL '+initial['hash'],flush=True);return 0
    finally:lab.stop();boundary.stop()
if __name__=='__main__':sys.exit(main())
