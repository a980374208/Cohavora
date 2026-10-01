"""In-memory matching of ephemeral test secrets; never persist needles/hashes."""
import base64
import json
from pathlib import Path


def scan_outputs(root: Path, secrets):
    if not root.is_dir(): raise ValueError('missing_output_tree')
    needles = set()
    for secret in secrets:
        if not secret: continue
        encoded = secret.encode('utf-8')
        needles.update((encoded, secret.encode('utf-16-le'), secret.encode('utf-16-be'),
                        base64.b64encode(encoded), json.dumps(secret, ensure_ascii=True)[1:-1].encode('ascii')))
    if not needles: raise ValueError('missing_scan_material')
    overlap = max(map(len, needles)) - 1
    files = matches = 0
    root = root.resolve()
    for path in root.rglob('*'):
        if path.is_symlink(): raise ValueError('unexpected_output_link')
        if not path.is_file(): continue
        if not path.resolve().is_relative_to(root): raise ValueError('output_escape')
        files += 1
        found = any(needle in str(path.relative_to(root)).encode('utf-8') for needle in needles)
        with path.open('rb') as source:
            tail = b''
            while chunk := source.read(65536):
                data = tail + chunk
                if any(needle in data for needle in needles): found = True
                tail = data[-overlap:] if overlap else b''
        matches += int(found)
    if not files: raise ValueError('empty_output_tree')
    return {'status': 'PASS' if matches == 0 else 'FAIL', 'files_scanned': files, 'files_with_secret': matches,
            'scope': 'specified fixture output tree; UTF-8/UTF-16/base64/JSON and names; no key hashes retained'}
