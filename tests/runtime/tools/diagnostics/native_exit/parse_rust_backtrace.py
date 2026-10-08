"""Extract bounded native PCs from remote-only Rust stderr, never panic payloads.

Mapped file offsets are translated through ELF PT_LOAD entries before addr2line.
The ready/before-stop maps are observations, not a crash-time mapping guarantee.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

MAX_BYTES = 1048576
MAX_FRAMES = 128
FRAME = re.compile(r'^\s*(\d+):\s+(0x[0-9a-fA-F]+)\s+-\s+')


def elf_address(path, file_offset):
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01':
            raise ValueError('unsupported_elf')
        values = struct.unpack('<16sHHIQQQIHHHHHH', header)
        phoff, phsize, phcount = values[5], values[9], values[10]
        if phsize != 56 or phcount > 128 or phoff > MAX_BYTES:
            raise ValueError('invalid_program_headers')
        stream.seek(phoff)
        raw = stream.read(phsize * phcount)
        if len(raw) != phsize * phcount:
            raise ValueError('truncated_program_headers')
    for ptype, flags, offset, vaddr, physical, filesz, memsz, align in struct.iter_unpack('<IIQQQQQQ', raw):
        if ptype == 1 and flags & 1 and offset <= file_offset < offset + filesz:
            return vaddr + file_offset - offset
    return None


def safe_symbol(value):
    # Tool-produced compiled symbol names only, never Rust's free-form panic text.
    if not value or value == '??' or len(value) > 512 or not value.isascii() or '://' in value:
        return None
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        return None
    return value


def extract(stderr, maps=None, symbolize=False):
    path = Path(stderr)
    with path.open('rb') as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError('stderr_budget_exceeded')
    text = raw.decode('utf-8', 'replace')
    snapshots = []
    if maps is not None and Path(maps).is_file():
        map_path = Path(maps)
        if map_path.stat().st_size > 262144:
            raise ValueError('maps_budget_exceeded')
        document = json.loads(map_path.read_text())
        snapshots = document.get('snapshots', [])
    frames, group, capped = [], -1, False
    symbol_deadline = time.monotonic() + 10
    for line in text.splitlines():
        if line.strip() == 'stack backtrace:':
            group += 1
        match = FRAME.match(line)
        if match is None or group < 0:
            continue
        if len(frames) == MAX_FRAMES:
            capped = True
            continue
        index, pc = int(match[1]), int(match[2], 16)
        frame = dict(backtrace_group=group, frame_index=index, pc=hex(pc),
                     module=None, mapping_stage=None, mapped_file_offset=None,
                     elf_address=None, symbol=None)
        for snapshot in reversed(snapshots):
            matching = [mapping for mapping in snapshot.get('mappings', [])
                if int(mapping['start']) <= pc < int(mapping['end'])]
            if len(matching) != 1:
                continue
            mapping = matching[0]
            module = mapping.get('path', '')
            frame['module'] = Path(module).name if module.startswith('/') else None
            frame['mapping_stage'] = snapshot.get('stage')
            file_offset = pc - int(mapping['start']) + int(mapping['offset'])
            frame['mapped_file_offset'] = hex(file_offset)
            if symbolize and time.monotonic() < symbol_deadline and module.startswith('/') and Path(module).is_file():
                try:
                    address = elf_address(module, file_offset)
                    frame['elf_address'] = hex(address) if address is not None else None
                    if address is not None and shutil.which('addr2line'):
                        result = subprocess.run(['addr2line', '-f', '-C', '-e', module, hex(address)],
                            capture_output=True, text=True, timeout=min(2, max(.01, symbol_deadline - time.monotonic())))
                        lines = result.stdout.splitlines()
                        if result.returncode == 0 and lines:
                            frame['symbol'] = safe_symbol(lines[0])
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    frame['symbol'] = None
            break
        frames.append(frame)
    panic = 'panicked at' in text or 'fatal runtime error' in text
    non_unwind = ('panic in a function that cannot unwind' in text or
                  'non-unwinding panic' in text)
    return dict(schema=1, diagnostic_only=True, stderr_bytes=len(raw),
        stderr_sha256=hashlib.sha256(raw).hexdigest(), rust_panic=panic,
        non_unwinding_panic=non_unwind, backtrace_groups=group + 1,
        status='NATIVE_RUST_BACKTRACE_CAPTURED' if frames else 'NO_NATIVE_STACK_INCONCLUSIVE',
        frames=frames, frame_cap_reached=capped, raw_panic_payload_exported=False,
        maps_are_pre_exit_observations=True, original_panic_initiator='NOT_PROVEN',
        native_exit_repair='NOT_PROVEN', qualification_credit=0, formal_credit=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stderr', type=Path, required=True)
    parser.add_argument('--maps', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--symbolize', action='store_true')
    args = parser.parse_args()
    try:
        result = extract(args.stderr, args.maps, args.symbolize)
    except Exception as exc:
        result = dict(status='INVALID_STACK_EVIDENCE', error_type=type(exc).__name__)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps({key: result[key] for key in ('status',) if key in result}))
    return 2 if result['status'] == 'INVALID_STACK_EVIDENCE' else 0


if __name__ == '__main__':
    raise SystemExit(main())
