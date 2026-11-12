#!/usr/bin/env python3
"""Offline Linux schema-2 DEB signing. Does not install, publish or configure APT."""
from __future__ import annotations
import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from urllib.parse import urlsplit
import update_manifest as common

TARGET = {'platform':'linux','architecture':'amd64','package_format':'deb',
          'distribution':'ubuntu','distribution_version':'24.04','package':'cohavora'}
ROOT_FIELDS = {'schema','product','channel','version','notes','published_at','force_update','linux-amd64-deb'}


def validate_package_version(value: object, version: str) -> str:
    if not isinstance(value,str) or not re.fullmatch(re.escape(version)+r'-[1-9][0-9]*',value):
        raise common.ManifestError('DEB package_version must equal app version plus a positive revision without leading zeroes')
    return value


def validate_payload(value: object) -> dict:
    if not isinstance(value,dict) or set(value) != ROOT_FIELDS or type(value.get('schema')) is not int or value['schema'] != 2:
        raise common.ManifestError('Linux payload requires exactly the schema-2 fields')
    artifact = value.get('linux-amd64-deb')
    if not isinstance(artifact,dict) or set(artifact) != set(TARGET) | {'package_version','size','sha256','urls'}:
        raise common.ManifestError('Invalid Linux artifact fields')
    if any(artifact.get(key) != expected for key,expected in TARGET.items()):
        raise common.ManifestError('Artifact is not Cohavora Ubuntu 24.04 amd64 DEB')
    version = common.validate_version(value.get('version'))
    validate_package_version(artifact.get('package_version'),version)
    # Reuse the established envelope-independent limits, text, version, URL,
    # digest and integer validation without changing the Windows schema parser.
    legacy = {key:val for key,val in value.items() if key != 'linux-amd64-deb'}
    legacy['schema']=1
    legacy['windows-x64']={key:artifact[key] for key in ('size','sha256','urls')}
    common.validate_payload(legacy)
    if any(urlsplit(url).scheme != 'https' for url in artifact['urls']):
        raise common.ManifestError('All Linux artifact URLs must use HTTPS')
    return value


def check_deb(path: Path, version: str) -> tuple[int,str,str]:
    descriptor=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC|os.O_NONBLOCK)
    with os.fdopen(descriptor,'rb') as stream:
        before=os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0<before.st_size<=common.MAX_INSTALLER_BYTES:
            raise common.ManifestError('DEB must be a bounded regular file')
        result = subprocess.run(['dpkg-deb','--field',f'/proc/self/fd/{stream.fileno()}',
                                 'Package','Version','Architecture'],pass_fds=(stream.fileno(),),
                                capture_output=True,text=True,timeout=30)
        if result.returncode:
            raise common.ManifestError('dpkg-deb rejected the package')
        fields = {}
        for line in result.stdout.splitlines():
            key,separator,value = line.partition(':')
            if not separator or key in fields: raise common.ManifestError('Invalid DEB identity fields')
            fields[key]=value.strip()
        package_version=validate_package_version(fields.get('Version'),version)
        if fields != {'Package':'cohavora','Version':package_version,'Architecture':'amd64'}:
            raise common.ManifestError('DEB Package, Version or Architecture does not match the Linux target')
        stream.seek(0)
        digest=hashlib.sha256()
        size=0
        while chunk:=stream.read(256*1024):
            size+=len(chunk)
            if size>common.MAX_INSTALLER_BYTES: raise common.ManifestError('DEB grew during hashing')
            digest.update(chunk)
        after=os.fstat(stream.fileno())
        if size!=before.st_size or any(getattr(before,key)!=getattr(after,key)
            for key in ('st_dev','st_ino','st_size','st_mtime_ns','st_ctime_ns')):
            raise common.ManifestError('DEB changed during metadata and digest validation')
        return size,digest.hexdigest(),package_version


def create(args) -> None:
    version=common.validate_version(args.version)
    size,digest,package_version=check_deb(args.deb,version)
    payload=validate_payload({'schema':2,'product':'Cohavora','channel':'stable','version':version,
        'notes':args.notes,'force_update':False,
        'published_at':args.published_at or dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'linux-amd64-deb':dict(TARGET,package_version=package_version,size=size,sha256=digest,urls=args.url)})
    encoded=json.dumps(payload,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()
    if len(encoded)>common.MAX_PAYLOAD_BYTES: raise common.ManifestError('Payload exceeds 48 KiB')
    signature=common.sign_payload(args.openssl,args.private_key,encoded)
    common.verify_signature(args.openssl,common.public_key_hex(args.openssl,args.private_key),encoded,signature)
    if args.output.resolve() in {args.deb.resolve(),args.private_key.resolve()}:
        raise common.ManifestError('Output must not overwrite package or private key')
    common.atomic_write(args.output,json.dumps({'payload':base64.b64encode(encoded).decode(),
        'signature':base64.b64encode(signature).decode()},separators=(',',':')).encode()+b'\n')
    print('Created signed Linux schema-2 manifest')


def verify(args) -> None:
    with args.manifest.open('rb') as stream:
        encoded=stream.read(common.MAX_ENVELOPE_BYTES+1)
    if len(encoded)>common.MAX_ENVELOPE_BYTES: raise common.ManifestError('Manifest exceeds 96 KiB')
    outer=common.load_json(encoded)
    if not isinstance(outer,dict) or set(outer)!={'payload','signature'}: raise common.ManifestError('Invalid envelope')
    payload=common.decode_base64(outer['payload'],'payload')
    signature=common.decode_base64(outer['signature'],'signature')
    if not payload or len(payload)>common.MAX_PAYLOAD_BYTES: raise common.ManifestError('Invalid payload size')
    common.verify_signature(args.openssl,args.public_key_hex,payload,signature)
    value=validate_payload(common.load_json(payload))
    if args.deb:
        size,digest,package_version=check_deb(args.deb,value['version'])
        artifact=value['linux-amd64-deb']
        if (size,digest,package_version)!=(artifact['size'],artifact['sha256'],artifact['package_version']):
            raise common.ManifestError('DEB bytes or package version do not match the signed manifest')
    print('Verified signed Linux schema-2 manifest')


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl',default='openssl')
    actions=parser.add_subparsers(dest='action',required=True)
    signing=actions.add_parser('create')
    signing.add_argument('--private-key',type=Path,required=True)
    signing.add_argument('--deb',type=Path,required=True)
    signing.add_argument('--version',required=True)
    signing.add_argument('--url',action='append',required=True)
    signing.add_argument('--notes',default='')
    signing.add_argument('--published-at')
    signing.add_argument('--output',type=Path,required=True)
    checking=actions.add_parser('verify')
    checking.add_argument('--manifest',type=Path,required=True)
    checking.add_argument('--public-key-hex',required=True)
    checking.add_argument('--deb',type=Path)
    args=parser.parse_args()
    try:
        create(args) if args.action=='create' else verify(args)
    except (common.ManifestError,OSError,UnicodeError,subprocess.SubprocessError):
        print('Linux manifest validation failed',file=sys.stderr)
        return 1
    return 0

if __name__=='__main__': raise SystemExit(main())
