"""Opt-in publisher cleanup tracing; never modifies the source or SDK on disk.

Usage: python publisher_exit_phases.py --diagnostic-exit-phases FILE -- SOURCE [args]
Missing terminal integrity record means incomplete evidence (including native abort).
"""
from __future__ import annotations
import argparse
import ast
import atexit
import contextlib
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time


class Recorder:
    MAX_RECORDS = 4096

    def __init__(self, path):
        self.fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self.active = False
        self.failed = False
        self.sequence = 0
        self.dropped = 0
        self.lock = threading.RLock()

    def emit(self, phase, kind, success=True):
        if not self.active:
            return
        try:
            with self.lock:
                # Reserve one record for terminal integrity.
                if self.sequence >= self.MAX_RECORDS - (phase != 'integrity'):
                    self.dropped += 1
                    self.failed = True
                    return
                self.sequence += 1
                row = dict(phase=phase, kind=kind, pid=os.getpid(),
                           tid=threading.get_native_id(), monotonic_ns=time.monotonic_ns(),
                           sequence=self.sequence, success=bool(success), dropped=self.dropped)
                data = (json.dumps(row, separators=(',', ':')) + '\n').encode('ascii')
                if os.write(self.fd, data) != len(data):
                    self.failed = True
        except BaseException:
            self.failed = True

    def activate(self):
        self.active = True
        self.emit('cleanup', 'begin')
        try:
            module = sys.modules.get('livekit.rtc._ffi_client')
            client = getattr(getattr(module, 'FfiClient', None), '_instance', None)
            if client is not None:
                self.wrap_ffi(client._ffi_lib)
            else:
                self.failed = True
            # Registered after SDK initialization: runs before SDK's disposal.
            atexit.register(self.python_atexit)
        except BaseException:
            self.failed = True

    def python_atexit(self):
        self.emit('python_atexit', 'begin')

    def finish(self):
        self.emit('integrity', 'end', not self.failed)

    @contextlib.contextmanager
    def phase(self, name):
        self.emit(name, 'begin')
        success = False
        try:
            yield
            success = True
        finally:
            self.emit(name, 'end', success)

    def wrap_ffi(self, library):
        for attr, phase in (('livekit_ffi_drop_handle', 'native_drop_handle'),
                            ('livekit_ffi_dispose', 'native_ffi_dispose')):
            original = getattr(library, attr)
            def wrapped(*args, _original=original, _phase=phase, **kwargs):
                with self.phase(_phase):
                    return _original(*args, **kwargs)
            setattr(library, attr, wrapped)

    async def publish(self, function, *args, **kwargs):
        success = False
        try:
            result = await function(*args, **kwargs)
            success = True
            return result
        finally:
            self.emit('publish_return', 'end', success)

    def runner(self, function, *args, **kwargs):
        success = False
        try:
            result = function(*args, **kwargs)
            success = True
            return result
        finally:
            self.emit('asyncio_run_return', 'end', success)


def instrument(source, filename):
    tree = ast.parse(source, filename)
    publish = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == 'publish')
    cleanup = next(n for n in publish.body if isinstance(n, ast.Try) and n.finalbody)
    body = cleanup.finalbody
    anchors = [i for i, n in enumerate(body) if isinstance(n, ast.For)]
    # Fail closed if the known capture/audio/room/source cleanup shape changes.
    if len(anchors) not in (4, 5) or not isinstance(body[-1], ast.Expr):
        raise ValueError('unsupported_publisher_cleanup_shape')
    # The cooperative-drain version cancels pending tasks in a nested failure
    # branch. Its four top-level loops begin with result inspection instead.
    if len(anchors) == 4:
        first = body[anchors[0]]
        if not isinstance(first.target, ast.Name) or first.target.id != 'result':
            raise ValueError('unsupported_publisher_cleanup_shape')
    offset = len(anchors) - 3
    cuts = [0, anchors[offset], anchors[offset+1], anchors[offset+2], len(body)-1, len(body)]
    names = ['capture_drain', 'audio_clear', 'room_disconnect', 'source_close', 'result_commit']
    # Keep complete/cadence computation in source-close group, in original order.
    wrapped = [ast.parse('_exit_recorder.activate()').body[0]]
    for name, start, end in zip(names, cuts, cuts[1:]):
        node = ast.parse(f'with _exit_recorder.phase({name!r}):\n pass').body[0]
        node.body = body[start:end]
        wrapped.append(node)
    cleanup.finalbody = wrapped
    class Calls(ast.NodeTransformer):
        def visit_Call(self, node):
            self.generic_visit(node)
            if isinstance(node.func, ast.Name) and node.func.id == 'publish':
                node.args.insert(0, node.func)
                node.func = ast.Attribute(ast.Name('_exit_recorder', ast.Load()), 'publish', ast.Load())
            elif (isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name)
                  and node.func.value.id == 'asyncio' and node.func.attr == 'run'):
                node.args.insert(0, node.func)
                node.func = ast.Attribute(ast.Name('_exit_recorder', ast.Load()), 'runner', ast.Load())
            return node
    return compile(ast.fix_missing_locations(Calls().visit(tree)), filename, 'exec')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--diagnostic-exit-phases', type=Path, required=True)
    parser.add_argument('--source-sha256')
    parser.add_argument('source', type=Path)
    parser.add_argument('source_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    path = args.source.resolve()
    source_bytes = path.read_bytes()
    if args.source_sha256 is not None and hashlib.sha256(source_bytes).hexdigest() != args.source_sha256:
        raise ValueError('publisher_source_sha256_mismatch')
    code = instrument(source_bytes.decode('utf-8'), str(path))
    recorder = Recorder(args.diagnostic_exit_phases)
    # First callback registered runs last, after SDK disposal.
    atexit.register(recorder.finish)
    sys.argv = [str(path), *args.source_args]
    sys.path.insert(0, str(path.parent))
    namespace = dict(__name__='__main__', __file__=str(path),
                     __package__=None, _exit_recorder=recorder)
    exec(code, namespace)


if __name__ == '__main__':
    main()
