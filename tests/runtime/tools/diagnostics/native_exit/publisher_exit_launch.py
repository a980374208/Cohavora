"""Load the exit observer in one owned worker, then remove preload inheritance.

No RTC import occurs before the observer handshake. The exec preserves PID and
start ticks; only the owned worker image loads the task's observation library.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys


BOOT = r'''
import ctypes, json, os, runpy, sys
from pathlib import Path
os.environ.pop('LD_PRELOAD', None)
observer = ctypes.CDLL(None)
ready = observer.native_exit_observer_ready
ready.argtypes = []; ready.restype = ctypes.c_int
if ready() != 1:
    print(json.dumps({'status':'OBSERVER_HANDSHAKE_FAILED'}), file=sys.stderr)
    raise SystemExit(2)
trace = Path(os.environ['NATIVE_EXIT_TRACE_DIR'])
with (trace/'observer-ready.json').open('x') as stream:
    json.dump({'status':'READY','pid':os.getpid(),
               'observer_sha256':os.environ['NATIVE_EXIT_OBSERVER_SHA'],
               'preload_in_child_environment':bool(os.environ.get('LD_PRELOAD')),
               'rtc_imported':False}, stream)
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name='__main__')
'''


def prepare(library: Path, expected: str, trace: Path, arguments: list[str]):
    if os.environ.get('LD_PRELOAD') or os.environ.get('LIVEKIT_LIB_PATH'):
        raise ValueError('parent_native_override_forbidden')
    if not re.fullmatch('[a-f0-9]{64}', expected):
        raise ValueError('observer_sha_invalid')
    matched = re.fullmatch(
        r'/root/livekit-product-acceptance/native-exit-([a-f0-9]{32})/'
        r'full-exit-capture/(?:subscriber-[12]/)?pthread-exit-trace', trace.as_posix())
    if not matched or trace.is_symlink() or trace.resolve() != trace:
        raise ValueError('observer_trace_scope_invalid')
    owner = Path('/root/livekit-product-acceptance/native-exit-' + matched[1])
    if (library.is_symlink() or library.resolve() != library
            or library.parent != owner / 'bundle'
            or hashlib.sha256(library.read_bytes()).hexdigest() != expected):
        raise ValueError('observer_library_binding_invalid')
    if not trace.is_dir() or any(trace.iterdir()):
        raise ValueError('observer_trace_directory_not_empty')
    if not arguments:
        raise ValueError('publisher_wrapper_missing')
    wrapper = Path(arguments[0])
    if wrapper.parent != owner / 'bundle' or wrapper.is_symlink() or not wrapper.is_file():
        raise ValueError('publisher_wrapper_scope_invalid')
    env = dict(os.environ, LD_PRELOAD=str(library),
               NATIVE_EXIT_TRACE_DIR=str(trace),
               NATIVE_EXIT_OBSERVER_SHA=expected, RUST_BACKTRACE='full')
    return [sys.executable, '-B', '-c', BOOT, *arguments], env


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--observer-library', type=Path, required=True)
    parser.add_argument('--observer-sha256', required=True)
    parser.add_argument('--trace-directory', type=Path,
                        default=Path(os.environ.get('NATIVE_EXIT_TRACE_DIR', '.')))
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    arguments = args.arguments
    if arguments[:1] == ['--']:
        arguments = arguments[1:]
    try:
        command, env = prepare(args.observer_library, args.observer_sha256,
                               args.trace_directory, arguments)
    except (OSError, ValueError) as error:
        print(json.dumps({'status':'OBSERVER_LAUNCH_REJECTED',
                          'error_type':type(error).__name__}))
        return 2
    os.execve(sys.executable, command, env)


if __name__ == '__main__':
    raise SystemExit(main())
