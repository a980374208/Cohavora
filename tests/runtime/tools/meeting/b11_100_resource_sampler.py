"""One-second private-veth and publisher/SFU counters for a namespace load."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import signal
import time

FIELDS = ('elapsed_s', 'publisher_cpu_ticks', 'publisher_rss_pages', 'sfu_cpu_ticks', 'sfu_rss_pages',
    'private_rx_bytes', 'private_tx_bytes', 'private_rx_packets', 'private_tx_packets')
STOP = False


def process_stat(pid, *, proc_root=Path('/proc')):
    fields = (proc_root / str(pid) / 'stat').read_text().rsplit(')', 1)[1].split()
    return {'start_ticks': fields[19], 'cpu_ticks': int(fields[11]) + int(fields[12]),
            'rss_pages': int(fields[21])}


def sample(args, start, *, proc_root=Path('/proc'), sys_root=Path('/sys/class/net')):
    publisher = process_stat(args.publisher_pid, proc_root=proc_root)
    sfu = process_stat(args.sfu_pid, proc_root=proc_root)
    interface = sys_root / args.private_interface
    if publisher['start_ticks'] != args.publisher_start_ticks or sfu['start_ticks'] != args.sfu_start_ticks or \
            int((interface / 'ifindex').read_text()) != args.private_ifindex or \
            (interface / 'ifalias').read_text().strip() != 'b11-owner:' + args.owner_id + ':host':
        raise RuntimeError('namespace_resource_identity_changed')
    stats = interface / 'statistics'
    return {'elapsed_s': round(time.monotonic() - start, 3),
        'publisher_cpu_ticks': publisher['cpu_ticks'], 'publisher_rss_pages': publisher['rss_pages'],
        'sfu_cpu_ticks': sfu['cpu_ticks'], 'sfu_rss_pages': sfu['rss_pages'],
        **{'private_' + name: int((stats / name).read_text())
           for name in ('rx_bytes', 'tx_bytes', 'rx_packets', 'tx_packets')}}


def stop(_signal, _frame):
    global STOP
    STOP = True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--publisher-pid', type=int, required=True)
    parser.add_argument('--publisher-start-ticks', required=True)
    parser.add_argument('--sfu-pid', type=int, required=True)
    parser.add_argument('--sfu-start-ticks', required=True)
    parser.add_argument('--private-interface', required=True)
    parser.add_argument('--private-ifindex', type=int, required=True)
    parser.add_argument('--owner-id', required=True)
    parser.add_argument('--duration', type=int, default=510)
    args = parser.parse_args(argv)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    start, count, error = time.monotonic(), 0, None
    try:
        with args.output.open('x', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS)
            writer.writeheader()
            while not STOP and time.monotonic() - start < args.duration:
                writer.writerow(sample(args, start))
                stream.flush()
                count += 1
                time.sleep(max(0, start + count - time.monotonic()))
    except Exception:
        error = 'namespace_resource_sample_failed'
    status = {'schema': 1, 'sample_count': count, 'error': error,
        'publisher_pid': args.publisher_pid, 'publisher_start_ticks': args.publisher_start_ticks,
        'sfu_pid': args.sfu_pid, 'sfu_start_ticks': args.sfu_start_ticks,
        'private_interface': args.private_interface, 'private_ifindex': args.private_ifindex,
        'owner_id': args.owner_id, 'stopped_by_signal': STOP}
    args.output.with_suffix('.status.json').write_text(json.dumps(status))
    return 1 if error else 0


if __name__ == '__main__':
    raise SystemExit(main())
