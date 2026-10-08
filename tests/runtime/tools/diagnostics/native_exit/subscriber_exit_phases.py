"""Opt-in frozen subscriber exit tracing; source and SDK remain unchanged on disk."""
from __future__ import annotations
import argparse
import ast
import atexit
import hashlib
from pathlib import Path
import sys
try:
    from publisher_exit_phases import Recorder as BaseRecorder
except ModuleNotFoundError:
    import importlib.util
    spec = importlib.util.spec_from_file_location('publisher_exit_phases', Path(__file__).with_name('publisher_exit_phases.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    BaseRecorder = module.Recorder

FROZEN_SOURCE_SHA256 = '288007e11f5bf0cc05a055b6c9a92759236a567126adf3dff368e4b32cdc82d0'

class Recorder(BaseRecorder):
    async def subscriber_run(self, function, *args, **kwargs):
        success = False
        try:
            result = await function(*args, **kwargs)
            success = result == 0
            return result
        finally:
            self.emit('subscriber_run_return', 'end', success)

    async def retirement(self, function, *args, **kwargs):
        success = False
        try:
            result = await function(*args, **kwargs)
            success = result == 0
            return result
        finally:
            self.emit('subscriber_retirement_return', 'end', success)

    def runner(self, function, *args, **kwargs):
        success = False
        try:
            result = function(*args, **kwargs)
            success = result == 0
            return result
        finally:
            self.emit('asyncio_run_return', 'end', success)


def _phase(name, body):
    node = ast.parse(f'with _exit_recorder.phase({name!r}):\n pass').body[0]
    node.body = body
    return node


def instrument(source, filename):
    tree = ast.parse(source, filename)
    try:
        run = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == 'run')
        cli = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef) and n.name == 'run_cli')
        cleanup = next(n for n in run.body if isinstance(n, ast.Try) and n.finalbody)
        retirement = next(n for n in cli.body if isinstance(n, ast.Try) and n.finalbody)
        body = cleanup.finalbody
        # Frozen statement layout: unsubscribe/cancel/gather, disconnect, result,
        # reference release. Reject structural drift before creating evidence.
        if len(body) != 13 or not isinstance(body[5], ast.Try):
            raise ValueError()
        anchors = [(1, 'room.off'), (3, 'asyncio.gather'), (8, 'atomic'),
                   (9, 'tasks.clear'), (10, 'results.clear')]
        for index, expected in anchors:
            node = body[index]
            call = node.value if isinstance(node, (ast.Expr, ast.Assign)) else None
            if isinstance(call, ast.Await):
                call = call.value
            if not isinstance(call, ast.Call) or ast.unparse(call.func) != expected:
                raise ValueError()
        disconnect = body[5]
        if ast.unparse(disconnect.body[0]) != 'await asyncio.wait_for(room.disconnect(), 5)':
            raise ValueError()
        if [ast.unparse(n) for n in retirement.finalbody[:4]] != [
                'gc.collect()', 'await asyncio.sleep(0)', 'gc.collect()', 'await asyncio.sleep(0)']:
            raise ValueError()
        if ast.unparse(retirement.finalbody[-1]) != "atomic(args.output / 'retirement.json', proof)":
            raise ValueError()
    except (StopIteration, ValueError, IndexError, AttributeError):
        raise ValueError('unsupported_subscriber_cleanup_shape') from None
    disconnect.body = [_phase('room_disconnect', disconnect.body)]
    cleanup.finalbody = [ast.parse('_exit_recorder.activate()').body[0],
        _phase('subscription_cancel_gather', body[:5]), disconnect,
        _phase('result_commit', body[6:9]), _phase('reference_clear', body[9:])]
    retirement.finalbody = [_phase('gc_weakref_retirement', retirement.finalbody)]
    class Calls(ast.NodeTransformer):
        def visit_Call(self, node):
            self.generic_visit(node)
            method = None
            if isinstance(node.func, ast.Name):
                method = {'run': 'subscriber_run', 'run_cli': 'retirement'}.get(node.func.id)
            elif ast.unparse(node.func) == 'asyncio.run':
                method = 'runner'
            if method:
                node.args.insert(0, node.func)
                node.func = ast.Attribute(ast.Name('_exit_recorder', ast.Load()), method, ast.Load())
            return node
    return compile(ast.fix_missing_locations(Calls().visit(tree)), filename, 'exec')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--diagnostic-exit-phases', type=Path, required=True)
    parser.add_argument('--source-sha256', default=FROZEN_SOURCE_SHA256)
    parser.add_argument('source', type=Path)
    parser.add_argument('source_args', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    path = args.source.resolve()
    source = path.read_bytes()
    if hashlib.sha256(source).hexdigest() != args.source_sha256:
        raise ValueError('subscriber_source_sha256_mismatch')
    code = instrument(source.decode('utf-8'), str(path))
    recorder = Recorder(args.diagnostic_exit_phases)
    atexit.register(recorder.finish)
    sys.argv = [str(path), *args.source_args]
    sys.path.insert(0, str(path.parent))
    exec(code, dict(__name__='__main__', __file__=str(path), __package__=None,
                   _exit_recorder=recorder))

if __name__ == '__main__':
    main()
