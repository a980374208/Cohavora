#!/usr/bin/env python3
"""Preserve the reviewed VAAPI DSO while CPack splits application debug symbols."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


RUNTIME_NAME = 'libcohavora_vaapi_runtime.so.1'


def sha(path: Path) -> str:
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(part)
    return result.hexdigest()


def no_links(path: Path, root: Path) -> None:
    if not path.is_relative_to(root):
        raise ValueError('path_outside_expected_stage')
    for parent in (*reversed(path.relative_to(root).parents), path.relative_to(root)):
        if (root / parent).is_symlink():
            raise ValueError('stage_link_rejected')


def debug_output(argument: str, stage: Path, *, existing: bool) -> Path:
    # CPack places the debug package beside the ordinary package staging root.
    root = stage.with_name(stage.name + '-dbgsym')
    path = Path(os.path.abspath(argument))
    no_links(path, root)
    relative = path.relative_to(root).as_posix()
    if not re.fullmatch(r'usr/lib/debug/\.build-id/[0-9a-fA-F]{2}/[0-9a-fA-F]+\.debug', relative):
        raise ValueError('unexpected_debug_output')
    if existing:
        if not path.is_file():
            raise ValueError('missing_debug_output')
    elif path.exists():
        raise ValueError('debug_output_already_exists')
    return path


def execute(real_objcopy: Path, expected: str, arguments: list[str]) -> int:
    mentions_runtime = any(Path(item.partition('=')[2] if item.startswith('-') and '=' in item else item).name
                           == RUNTIME_NAME for item in arguments)
    if not mentions_runtime:
        return subprocess.run([str(real_objcopy), *arguments], check=False).returncode

    extract = len(arguments) == 3 and arguments[0] == '--only-keep-debug'
    strip = len(arguments) == 2 and arguments[0] == '--strip-unneeded'
    link = len(arguments) == 2 and arguments[0].startswith('--add-gnu-debuglink=')
    if not (extract or strip or link):
        raise ValueError('unsupported_immutable_runtime_operation')
    source = arguments[1]
    if '..' in Path(source).parts:
        raise ValueError('runtime_path_traversal_rejected')
    stage = Path.cwd().resolve()
    runtime = stage / 'usr/lib/cohavora' / RUNTIME_NAME
    if Path(os.path.abspath(source)) != runtime:
        raise ValueError('unexpected_runtime_stage_path')
    no_links(runtime, stage)
    descriptor = os.open(runtime, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise ValueError('runtime_is_not_regular')
        def unchanged() -> None:
            current = runtime.stat(follow_symlinks=False)
            if (not stat.S_ISREG(current.st_mode)
                    or (current.st_dev, current.st_ino) != (opened.st_dev, opened.st_ino)
                    or sha(runtime) != expected):
                raise ValueError('immutable_runtime_identity_mismatch')
        unchanged()
        if extract:
            debug_output(arguments[2], stage, existing=False)
            result = subprocess.run([str(real_objcopy), *arguments], check=False).returncode
        else:
            if link:
                debug_output(arguments[0].partition('=')[2], stage, existing=True)
            # The SDK DSO is immutable. Its symbols may be extracted, but its
            # original bytes and SDK-profile SHA must remain in the payload.
            result = 0
        unchanged()
        return result
    finally:
        os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objcopy', required=True, type=Path)
    parser.add_argument('--runtime-sha256', required=True)
    parser.add_argument('--guard-sha256', required=True)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not all(re.fullmatch('[0-9a-f]{64}', value)
               for value in (args.runtime_sha256, args.guard_sha256)):
        raise ValueError('invalid_expected_identity')
    own_source = Path(__file__).resolve()
    if sha(own_source) != args.guard_sha256:
        raise ValueError('guard_source_identity_mismatch')
    real_objcopy = args.objcopy.resolve(strict=True)
    if not args.objcopy.is_absolute() or not real_objcopy.is_file() or not os.access(real_objcopy, os.X_OK):
        raise ValueError('invalid_objcopy_tool')
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    if not arguments:
        raise ValueError('missing_objcopy_arguments')
    result = execute(real_objcopy, args.runtime_sha256, arguments)
    if sha(own_source) != args.guard_sha256:
        raise ValueError('guard_source_identity_changed')
    return result


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        # Fixed reasons only; CPack already retains its command when it fails.
        print('Cohavora objcopy guard rejected operation: ' +
              (str(error) if isinstance(error, ValueError) else 'filesystem_error'), file=sys.stderr)
        sys.exit(1)
