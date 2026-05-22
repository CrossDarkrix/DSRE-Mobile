#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, os, platform, shutil, subprocess, sys
from pathlib import Path
from typing import Dict, List, Optional
ROOT=Path(__file__).resolve().parent
CONFIG_FILE=ROOT/'build.local.json'
SUPPORTED_ABIS=('arm64-v8a','armeabi-v7a')
DEFAULT_ANDROID_API={'arm64-v8a':'28','armeabi-v7a':'24'}
FFMPEG_SCRIPT={'arm64-v8a':'build_ffmpeg_android_aarch64.sh','armeabi-v7a':'build_ffmpeg_android_armv7abi.sh'}
def say(s:str)->None: print(s,flush=True)
def fail(s:str,code:int=2)->None: print('ERROR: '+s,file=sys.stderr); raise SystemExit(code)
def run(cmd:List[str],cwd:Optional[Path]=None,env:Optional[Dict[str,str]]=None,dry_run:bool=False)->None:
    say('+ '+' '.join(map(str,cmd))+(f'  # cwd={cwd}' if cwd else ''))
    if not dry_run: subprocess.run(cmd,cwd=str(cwd) if cwd else None,env=env,check=True)
def load_config()->Dict[str,object]:
    if CONFIG_FILE.exists():
        with CONFIG_FILE.open('r',encoding='utf-8') as f:
            d=json.load(f)
        return d if isinstance(d,dict) else {}
    return {}
def save_config(c:Dict[str,object])->None:
    with CONFIG_FILE.open('w',encoding='utf-8') as f: json.dump(c,f,indent=2,ensure_ascii=False); f.write('\n')
    say(f'Wrote {CONFIG_FILE}')
def detect_host_tag()->str:
    s=platform.system().lower(); m=platform.machine().lower()
    if s=='linux': return 'linux-x86_64'
    if s=='darwin': return 'darwin-arm64' if m in ('arm64','aarch64') else 'darwin-x86_64'
    if s=='windows' or s.startswith('msys') or s.startswith('cygwin'): return 'windows-x86_64'
    return 'linux-x86_64'
def resolve_ndk(v:Optional[str],c:Dict[str,object])->Path:
    raw=v or os.environ.get('ANDROID_NDK') or os.environ.get('NDK') or str(c.get('ndk',''))
    if not raw: fail('Android NDK path is not set. Use --ndk, ANDROID_NDK, NDK, or configure first.')
    p=Path(os.path.expanduser(str(raw))).resolve()
    if not p.is_dir(): fail(f'Android NDK path does not exist: {p}')
    if not (p/'toolchains'/'llvm'/'prebuilt').is_dir(): fail(f'NDK LLVM prebuilt directory not found under: {p}')
    return p
def parse_abis(t:Optional[str])->List[str]:
    if not t or t=='all': return list(SUPPORTED_ABIS)
    xs=[x.strip() for x in t.split(',') if x.strip()]
    for x in xs:
        if x not in SUPPORTED_ABIS: fail(f"Unsupported ABI: {x}. Supported: {', '.join(SUPPORTED_ABIS)}")
    return xs
def env_for(c:Dict[str,object],a:argparse.Namespace,abi:str)->Dict[str,str]:
    e=os.environ.copy(); ndk=resolve_ndk(getattr(a,'ndk',None),c)
    e['ANDROID_NDK']=str(ndk); e['NDK']=str(ndk)
    e['HOST_TAG']=getattr(a,'host_tag',None) or os.environ.get('HOST_TAG') or str(c.get('host_tag') or detect_host_tag())
    e['ANDROID_API']=str(getattr(a,'android_api',None) or os.environ.get('ANDROID_API') or c.get('android_api') or DEFAULT_ANDROID_API[abi])
    e['ANDROID_PLATFORM']=str(getattr(a,'android_platform',None) or c.get('android_platform') or 'android-24')
    e['BUILD_TYPE']=str(getattr(a,'build_type',None) or c.get('build_type') or 'Release')
    return e
def lame_prefix(a:argparse.Namespace,c:Dict[str,object],abi:str)->Path:
    if getattr(a,'lame_prefix',None): return Path(os.path.expanduser(a.lame_prefix)).resolve()
    root=getattr(a,'lame_root',None) or c.get('lame_root')
    return (Path(os.path.expanduser(str(root))).resolve()/abi) if root else (ROOT/'lame-3.100'/'android-build'/abi).resolve()
def ffmpeg_prefix(a:argparse.Namespace,c:Dict[str,object],abi:str)->Path:
    return Path(os.path.expanduser(a.ffmpeg_prefix)).resolve() if getattr(a,'ffmpeg_prefix',None) else (ROOT/'android-build'/abi).resolve()
def sync_ffmpeg(prefix:Path,abi:str,dry:bool=False)->None:
    if not (prefix/'include').is_dir() or not (prefix/'lib').is_dir(): fail(f'FFmpeg output is incomplete for {abi}: {prefix} (expected include/ and lib/)')
    dest=ROOT/'dsre_native'/'ffmpeg'/abi; say(f'Sync FFmpeg: {prefix} -> {dest}')
    if dry: return
    dest.mkdir(parents=True,exist_ok=True)
    for n in ('include','lib'):
        if (dest/n).exists(): shutil.rmtree(dest/n)
        shutil.copytree(prefix/n,dest/n)
def cmd_configure(a):
    c=load_config()
    if a.ndk or os.environ.get('ANDROID_NDK') or os.environ.get('NDK'): c['ndk']=str(resolve_ndk(a.ndk,c))
    c['host_tag']=a.host_tag or str(c.get('host_tag') or detect_host_tag())
    if a.android_api: c['android_api']=str(a.android_api)
    c['android_platform']=a.android_platform; c['build_type']=a.build_type
    if a.ffmpeg_source: c['ffmpeg_source']=str(Path(os.path.expanduser(a.ffmpeg_source)).resolve())
    if a.lame_root: c['lame_root']=str(Path(os.path.expanduser(a.lame_root)).resolve())
    save_config(c)
def cmd_ffmpeg(a):
    c=load_config(); src_raw=a.ffmpeg_source or c.get('ffmpeg_source')
    if not src_raw: fail('FFmpeg source directory is not set. Use --ffmpeg-source or configure first.')
    src=Path(os.path.expanduser(str(src_raw))).resolve()
    if not (src/'configure').is_file(): fail(f'FFmpeg configure script not found: {src/"configure"}')
    for abi in parse_abis(a.abis or a.abi):
        e=env_for(c,a,abi); lp=lame_prefix(a,c,abi); fp=ffmpeg_prefix(a,c,abi)
        if not lp.is_dir(): fail(f'LAME_PREFIX for {abi} does not exist: {lp}')
        e['LAME_PREFIX']=str(lp); e['FFMPEG_PREFIX']=str(fp)
        run(['sh',str(ROOT/FFMPEG_SCRIPT[abi])],cwd=src,env=e,dry_run=a.dry_run)
        if a.sync_native: sync_ffmpeg(fp,abi,a.dry_run)
def cmd_sync(a):
    c=load_config()
    for abi in parse_abis(a.abis or a.abi): sync_ffmpeg(ffmpeg_prefix(a,c,abi),abi,a.dry_run)
def cmd_native(a):
    c=load_config()
    for abi in parse_abis(a.abis or a.abi):
        e=env_for(c,a,abi); e['ABI']=abi
        run(['sh',str(ROOT/'build_libdsre_audio_c.sh')],cwd=ROOT,env=e,dry_run=a.dry_run)
def cmd_all(a):
    a.abis=','.join(parse_abis(a.abis)); a.sync_native=True; cmd_ffmpeg(a); cmd_native(a)
def cmd_clean(a):
    for p in [ROOT/'android-build',ROOT/'native_libs',ROOT/'dsre_native'/'ffmpeg',ROOT/'dsre_native'/'build-arm64-v8a',ROOT/'dsre_native'/'build-armeabi-v7a']:
        if p.exists(): say(f'remove {p}'); (None if a.dry_run else shutil.rmtree(p))
def add_common(p):
    p.add_argument('--ndk'); p.add_argument('--host-tag'); p.add_argument('--android-api'); p.add_argument('--android-platform',default='android-24'); p.add_argument('--build-type',default='Release'); p.add_argument('--dry-run',action='store_true')
def make_parser():
    p=argparse.ArgumentParser(description='Unified DSRE-Mobile Android build driver'); sub=p.add_subparsers(dest='command',required=True)
    c=sub.add_parser('configure',help='write machine-local build.local.json'); c.add_argument('--ndk'); c.add_argument('--host-tag'); c.add_argument('--android-api'); c.add_argument('--android-platform',default='android-24'); c.add_argument('--build-type',default='Release'); c.add_argument('--ffmpeg-source'); c.add_argument('--lame-root'); c.set_defaults(func=cmd_configure)
    f=sub.add_parser('ffmpeg',help='build FFmpeg for one or more ABIs'); add_common(f); f.add_argument('--abi',choices=SUPPORTED_ABIS); f.add_argument('--abis'); f.add_argument('--ffmpeg-source'); f.add_argument('--lame-prefix'); f.add_argument('--lame-root'); f.add_argument('--ffmpeg-prefix'); f.add_argument('--sync-native',action='store_true'); f.set_defaults(func=cmd_ffmpeg)
    s=sub.add_parser('sync-ffmpeg',help='copy FFmpeg output into dsre_native/ffmpeg/<ABI>'); add_common(s); s.add_argument('--abi',choices=SUPPORTED_ABIS); s.add_argument('--abis'); s.add_argument('--ffmpeg-prefix'); s.set_defaults(func=cmd_sync)
    n=sub.add_parser('native',help='build libdsre_audio.so'); add_common(n); n.add_argument('--abi',choices=SUPPORTED_ABIS); n.add_argument('--abis'); n.set_defaults(func=cmd_native)
    a=sub.add_parser('all',help='build FFmpeg, sync it, then build native libraries'); add_common(a); a.add_argument('--abis',default='all'); a.add_argument('--ffmpeg-source'); a.add_argument('--lame-prefix'); a.add_argument('--lame-root'); a.add_argument('--ffmpeg-prefix'); a.set_defaults(func=cmd_all)
    cl=sub.add_parser('clean',help='remove generated build outputs'); cl.add_argument('--dry-run',action='store_true'); cl.set_defaults(func=cmd_clean)
    return p
def main(argv:Optional[List[str]]=None)->int:
    a=make_parser().parse_args(argv)
    try: a.func(a); return 0
    except subprocess.CalledProcessError as e: fail(f"command failed with exit code {e.returncode}: {' '.join(e.cmd)}",e.returncode or 1)
if __name__=='__main__': raise SystemExit(main())
