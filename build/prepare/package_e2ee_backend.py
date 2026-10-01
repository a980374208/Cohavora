"""Package the reviewed native E2EE overlay, without editing the supplied SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

PROFILE = Path(__file__).with_name('e2ee-backend-profile.json')


def assemble(patched, guard, bsd_license, apache_license):
    profile = json.loads(PROFILE.read_text(encoding='utf-8'))
    payload = {}
    for name, expected in profile['files'].items():
        if name.startswith('patched/'): source = patched / name.removeprefix('patched/')
        elif name.startswith('media-guard/'): source = guard / name.removeprefix('media-guard/')
        elif name == 'licenses/WebRTC-BSD.txt': source = bsd_license
        elif name == 'licenses/Apache-2.0.txt': source = apache_license
        else: raise ValueError('unsupported profile entry')
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected: raise ValueError('profile mismatch: ' + name)
        payload[name] = data
    payload['package.json'] = (json.dumps(profile, sort_keys=True, indent=2) + '\n').encode()
    payload['NOTICE.txt'] = (
        'Cohavora E2EE native overlay. Derived from WebRTC ' + profile['upstream_commit'] + '.\n'
        'Crypto transformer: Copyright 2022 LiveKit, Apache-2.0. Channel sources: WebRTC BSD.\n'
        'Modified: conditional ratchet commits, bounded receive work, required media guards, bounded process IV allocation.\n'
        'Use only with ' + profile['sdk_package_id'] + '; this is not a standalone SDK.\n'
        'Original source notices are retained. Base SDK licenses remain separately applicable.\n'
    ).encode()
    return profile, payload


def package(args):
    profile, payload = assemble(args.patched, args.guard, args.bsd_license, args.apache_license)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    package_root = output / profile['package_id']
    for name, data in payload.items():
        target = package_root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    archive = output / (profile['package_id'] + '.zip')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED) as stream:
        for name in sorted(payload):
            entry = zipfile.ZipInfo(profile['package_id'] + '/' + name, (2026, 10, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            stream.writestr(entry, payload[name])
    result = {'package_id': profile['package_id'], 'directory': str(package_root),
        'archive': str(archive), 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'scope': 'local reproducible source overlay; not published; runtime verdict separate'}
    (output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('patched', 'guard', 'bsd-license', 'apache-license', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    package(parser.parse_args())
