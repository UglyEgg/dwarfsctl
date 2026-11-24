#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public CLI migration contracts; disposable images/config, no host mutations."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import hashlib
import fcntl

cli, engine = (str(Path(p).resolve()) for p in sys.argv[1:3])
checks = 0
with tempfile.TemporaryDirectory(prefix='dwarfsctl-cpp-') as tmp:
    root = Path(tmp)
    runtime = root / 'run'
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, HOME=tmp, XDG_CONFIG_HOME=str(root/'config'), XDG_CACHE_HOME=str(root/'cache'),
               XDG_RUNTIME_DIR=str(runtime), XDG_DATA_HOME=str(root/'data'), PATH='/nonexistent', DWARFSCTL_ENGINE=engine, DBUS_SESSION_BUS_ADDRESS='unix:path='+str(root/'absent-bus'))
    config = root/'override.toml'
    config.write_text('[ui]\nopen_after_mount=true\n')

    def run(*args, code=0, override=config, run_env=None):
        global checks
        cmd = [cli] + (['--config', str(override)] if override else []) + list(map(str,args))
        p = subprocess.run(cmd, env=run_env or env, capture_output=True)
        doc=json.loads(p.stdout)
        assert p.returncode==code, (cmd,p.returncode,doc,p.stderr)
        assert doc['ok']==(code==0),doc
        assert set(('ok','command','image','details','warnings','warning_objects','error')) <= doc.keys()
        if code: assert doc['error']['code']==code
        checks+=1
        return doc['details'] if code==0 else doc

    cfg=run('config','show')
    assert 'places' not in cfg
    assert cfg['ui']['command_timeout_ms']==300000
    assert 'paths' not in cfg
    run('config','show','--section','unknown',code=2)
    user=root/'config/dwarfs-dolphin/config.toml'
    user.parent.mkdir(parents=True)
    user.write_text('[ui]\nopen_after_mount=false\ncommand_timeout_ms=123\n[places]\nwrite_sidecar=false\n')
    assert run('config','show',override=None)['ui']['open_after_mount'] is False
    config.write_text('[ui]\nopen_after_mount=true\n')
    merged=run('config','show');assert merged['ui']['open_after_mount'] is True and merged['ui']['command_timeout_ms']==123
    assert 'places' not in merged
    invalid=root/'invalid.toml';invalid.write_text('[ui]\ncommand_timeout_ms="no"\n')
    run('config','show',override=invalid,code=2)
    run('config','show',override=root/'missing',code=2)
    unknown=root/'unknown.toml';unknown.write_text('[wat]\nx=1\n')
    p=subprocess.run([cli,'--config',str(unknown),'config','show'],env=env,capture_output=True)
    assert json.loads(p.stdout)['warning_objects'][0]['code']=='config_unknown_key'
    saved=run('config','set',
              'ui.open_after_mount','true',
              'ui.command_timeout_ms','45000',
              'ui.build_timeout_ms','0',
              'creation.default_mode','recommend',
              'creation.default_profile','@balanced',
              'creation.recommendation_goal','smallest',override=None)
    assert saved['preferences']['creation.recommendation_goal']=='smallest'
    saved_config=run('config','show',override=None)
    assert saved_config['ui']['command_timeout_ms']==45000
    assert saved_config['creation']=={'default_mode':'recommend','default_profile':'@balanced','recommendation_goal':'smallest'}
    assert 'write_sidecar' not in user.read_text() and '[places]' not in user.read_text()
    run('config','set','ui.open_after_mount','maybe',override=None,code=2)
    run('config','set','places.write_sidecar','true',override=None,code=2)
    run('config','set','creation.recommendation_goal','impossible',override=None,code=2)
    run('config','set','paths.mount_root','/tmp/nope',override=None,code=2)
    profile=('config','mkdwarfs-profiles')
    assert run(*profile,'list')['profiles']==[]
    assert '@balanced' in run(*profile,'list','--all')['profiles']
    assert run(*profile,'get','default')['profile']['args']==[]
    run(*profile,'get','missing',code=4)
    for name in ('@balanced','default','Default',' '):
        run(*profile,'set',name,'--arg=--compress-level=1',code=2)
    for values in (['--input=/bad'],['--output=/bad'],['--force'],['--workers=3'],['--metadata-compression'],
                   ['--metadata-compression','--help'],['--compress-level=99'],['--compression=zstd:level=99'],
                   ['--order=oops'],['--progress=oops'],['--incompressible-ratio=nan']):
        run(*profile,'validate',*[f'--arg={v}' for v in values],code=2)
    for values in (['--compression=zstd:level=-1'],['--compress-level','1'],['--metadata-compression=zstd:level=22'],
                   ['--input-list=files.txt'],['--recompress'],['--filter','+ *.txt']):
        run(*profile,'validate',*[f'--arg={v}' for v in values])
    run(*profile,'set','test','--arg=--compress-level=1','--arg=--compression=zstd:level=1')
    assert run(*profile,'get','test')['profile']['args']==['--compress-level=1','--compression=zstd:level=1']
    assert run('config','show',override=None)['ui']['command_timeout_ms']==45000
    run(*profile,'delete','test')
    run(*profile,'delete','test',code=4)
    schema=run('mkdwarfs','schema','--include-long-help')
    assert '--compression' in schema['mkdwarfs_long_help']
    assert schema['schema']['schema_version']==1
    assert all(t['found'] for t in run('tools','schema')['tools'])
    src=root/'source';src.mkdir();(src/'sub').mkdir()
    (src/'hello.txt').write_text('hello\n');(src/'sub/data.bin').write_bytes(bytes(range(256))*2048)
    for i in range(12):(src/f'f{i}.txt').write_text('some data'*20)
    img=root/'image.dwarfs'
    args=('build',src,'--output',img,'--mkdwarfs-arg=--compress-level=1')
    plan=run(*args,'--dry-run');assert plan['output_verified'] and not img.exists()
    run(*args,'--dry-run','--preview',code=2)
    measured=run(*args,'--preview')
    assert not measured['output_verified'] and measured['preview_verified']
    assert measured['summary']['preview'] and measured['summary']['image_bytes']>0
    assert measured['summary']['files']==14 and measured['summary']['original_bytes']>0
    assert any(row['type']=='.txt' and row['files']>=13 for row in measured['summary']['source_types'])
    assert sum(row['files'] for row in measured['summary']['source_types'])==14
    assert sum(row['original_bytes'] for row in measured['summary']['source_types'])==measured['summary']['original_bytes']
    assert not img.exists() and not list(root.glob('.dwarfsctl-*'))
    with tempfile.TemporaryDirectory(prefix='.dwarfsctl-preview-', dir=root) as stage_name:
        stage=Path(stage_name)
        stage.chmod(0o1700)
        stale=root/'.dwarfsctl-preview-stale1'
        active=root/'.dwarfsctl-preview-active'
        unmarked=root/'.dwarfsctl-preview-old123'
        for sibling in (stale, active, unmarked):
            sibling.mkdir(mode=0o700)
            (sibling/'candidate.dwarfs').write_bytes(b'temporary')
        stale.chmod(0o1700)
        (stale/'.dwarfsctl-123456').write_bytes(b'interrupted build')
        active.chmod(0o1700)
        directory_fd=os.open(active, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(directory_fd, fcntl.LOCK_SH)
            run(*args,'--preview','--preview-stage',stage)
            assert not stale.exists()
            assert (active/'candidate.dwarfs').is_file()
            assert (unmarked/'candidate.dwarfs').is_file()
        finally:
            os.close(directory_fd)
        retained=run(*args,'--preview','--preview-stage',stage)
        assert not active.exists()
        (unmarked/'candidate.dwarfs').unlink()
        unmarked.rmdir()
        candidate=Path(retained['preview_candidate'])
        assert candidate==stage/'candidate.dwarfs'
        assert candidate.is_file() and candidate.stat().st_size==retained['summary']['image_bytes']
        assert not img.exists()
        run('build',src,'--output',img,'--publish-preview',candidate,
            '--expected-size',candidate.stat().st_size+1,code=4)
        assert not img.exists()
        published=run('build',src,'--output',img,'--publish-preview',candidate,
                      '--expected-size',candidate.stat().st_size)
        assert published['published_preview'] and published['output_verified']
        assert img.stat().st_size==retained['summary']['image_bytes']
        assert os.stat(img).st_ino==os.stat(candidate).st_ino
    assert img.is_file() and not stage.exists()
    nested_destination=src/'sub[1]*'
    nested_destination.mkdir()
    with tempfile.TemporaryDirectory(prefix='.dwarfsctl-preview-', dir=nested_destination) as stage_name:
        stage=Path(stage_name)
        stage.chmod(0o1700)
        nested=run('build',src,'--output',nested_destination/'image.dwarfs',
                   '--preview','--preview-stage',stage)
        assert nested['summary']['files']==14, nested['summary']
    nested_destination.rmdir()
    run('build',src,'--output',img,'--preview-stage',root,code=2)
    run('build',src,'--output',img,'--publish-preview',root/'missing',code=2)
    img.unlink()
    built=run(*args);assert built['output_verified'] and img.stat().st_size>0
    assert built['summary']['files']==14 and built['summary']['image_bytes']==img.stat().st_size
    assert built['summary']['original_bytes']>0 and built['summary']['elapsed_ms']>=0
    before=hashlib.sha256(img.read_bytes()).hexdigest()
    repeated=run(*args,'--preview')
    assert repeated['summary']['preview'] and hashlib.sha256(img.read_bytes()).hexdigest()==before
    assert not list(root.glob('.dwarfsctl-*'))
    # Forced publication replaces the requested symlink, never its target.
    build_victim=root/'build-victim';build_victim.write_bytes(b'keep')
    build_link=root/'build-link.dwarfs';build_link.symlink_to(build_victim)
    run('build',src,'--output',build_link,code=2)
    run('build',src,'--output',build_link,'--force','--mkdwarfs-arg=--compress-level=1')
    assert build_victim.read_bytes()==b'keep'
    assert not build_link.is_symlink() and build_link.read_bytes()[:6]==b'DWARFS'
    # Engine progress must reach the caller before the writer finishes.
    import select, shlex, time
    wrapper=root/'slow-engine'
    wrapper.write_text('#!/bin/sh\nif [ "$1" = mkdwarfs ]; then echo "scanning source" >&2; sleep 1; fi\nexec '+shlex.quote(engine)+' "$@"\n')
    wrapper.chmod(0o700)
    stream_env=dict(env, DWARFSCTL_ENGINE=str(wrapper), PATH='/usr/bin:/bin')
    proc=subprocess.Popen([cli,'--config',str(config),'build',str(src),'--output',str(root/'streamed.dwarfs')],env=stream_env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
    observed=False
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        if not select.select([proc.stderr],[],[],1)[0]: continue
        line=proc.stderr.readline()
        if not line: break
        if b'"phase":"scanning"' in line:
            assert proc.poll() is None
            observed=True
            break
    stdout,stderr=proc.communicate(timeout=30)
    assert observed and proc.returncode==0,(stdout,stderr)
    assert json.loads(stdout)['details']['summary']['files']==14
    assert not Path(str(img)+'.meta.json').exists()
    import re
    advertised={o['opt'] for c in schema['schema']['categories'] for o in c['options']}
    actual=set(re.findall(r'^  (?:-[A-Za-z] \[ )?(--[a-z][a-z-]+)',schema['mkdwarfs_long_help'],re.M))
    assert advertised == actual - {'--input','--output','--force','--help','--long-help','--man'}
    assert schema['supported_mkdwarfs_version']=='0.15.7'
    for name in ('@balanced','@fast','@max','@git'):
        flags=run(*profile,'get',name)['profile']['args']
        run('build',src,'--output',root/(name+'.dwarfs'),*[f'--mkdwarfs-arg={v}' for v in flags])
    run('ls','--','--config',code=4)
    run('ls','--','--help',code=4)
    before=img.read_bytes();run(*args,code=2)
    run(*args,'--force','--mkdwarfs-arg=--input-list=/nonexistent/list',code=9)
    assert img.read_bytes()==before
    lockfile=runtime/'dwarfs/locks'/(hashlib.sha256(str(img.resolve()).encode()).hexdigest()+'.lock')
    with lockfile.open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        run(*args,'--force',code=5)
    run(*args,'--force','--mkdwarfs-arg=--compression=not-real',code=2)
    assert img.read_bytes()==before
    run(*args,'--overwrite-output')
    status=run('status',img)
    assert status['dwarfsck']['json']
    assert status['mount_state_known'] is False and 'mounted' not in status
    assert status['mountpoint'] is None
    assert run('status')['deps']['mkdwarfs']['found']
    run('fsck',img)
    checksums=root/'checksums.sha256'
    run('checksum','export',img,'--algo','sha256','--output',checksums)
    assert 'hello.txt' in checksums.read_text()
    checksum_victim=root/'checksum-victim';checksum_victim.write_text('keep')
    checksum_link=root/'checksum-link';checksum_link.symlink_to(checksum_victim)
    run('checksum','export',img,'--algo','sha256','--output',checksum_link)
    assert checksum_victim.read_text()=='keep'
    assert not checksum_link.is_symlink() and 'hello.txt' in checksum_link.read_text()
    run('checksum','export',img,'--algo','sha256','--output',img,code=2)
    assert img.read_bytes()[:6]==b'DWARFS'
    verifyenv=dict(env,PATH='/usr/bin:/bin')
    run('checksum','verify',img,'--algo','sha256','--input',checksums,'--dir',src,run_env=verifyenv)
    (src/'hello.txt').write_text('changed')
    run('checksum','verify',img,'--algo','sha256','--input',checksums,'--dir',src,run_env=verifyenv,code=9)
    complete=root/'complete-extraction'
    extracted=run('extract',img,complete)
    assert extracted['dest']==str(complete)
    assert (complete/'hello.txt').read_text()=='hello\n'
    assert (complete/'sub/data.bin').read_bytes()==bytes(range(256))*2048
    collision=root/'collision';collision.mkdir()
    (collision/'hello.txt').write_text('keep')
    run('extract',img,collision,code=4)
    assert (collision/'hello.txt').read_text()=='keep'
    out=root/'selected'
    run('extract',img,out,'--arg=--pattern','--arg=hello.txt')
    assert (out/'hello.txt').read_text()=='hello\n' and not (out/'sub/data.bin').exists()
    for goal in ('smallest','balanced','fastest-browse','fast-build'):
        rec=run('mkdwarfs','recommend',src,'--goal',goal,'--cache','off')
        assert rec['schema']=='dwarfsctl.recommend.v1' and rec['goal']==goal and not rec['cache_hit']
        assert rec['recommendation']['notes'] and not rec['recommendation']['warnings']
        output=root/(goal+'.dwarfs')
        run('build',src,'--output',output,*[f'--mkdwarfs-arg={flag}' for flag in rec['recommendation']['flags_only']])
        run('fsck',output)
    first=run('mkdwarfs','recommend',src)
    second=run('mkdwarfs','recommend',src);assert second['cache_hit']
    (src/'newfile').write_text('a')
    assert not run('mkdwarfs','recommend',src)['cache_hit']
    run('mkdwarfs','recommend',root/'absent',code=4)
    run('mkdwarfs','recommend',src,'--goal','oops',code=2)
    run('mkdwarfs','recommend',src,'--cache','oops',code=2)
    run('mount','list',code=3,run_env=dict(env,DBUS_SESSION_BUS_ADDRESS='unix:path=/no/bus'))
    assert run('mount','registry-path')['path']==str(runtime/'dwarfsctl/mounts.json')
    for invalid_args in (['wat'],['cat'],['build',src],['ls',img,'--unknown'],['ls',img,'--entry-only'],
                         ['unmount'],['unmount','x','--image','y'],['config'],['checksum','verify',img,'--algo','sha256']):
        run(*invalid_args,code=2)
print(f'{checks} C++ public CLI contracts passed')
