"""Sample numeric ECS host, LiveKit process, and network counters for a soak probe."""
from __future__ import print_function

import argparse
import csv
import datetime
import json
import os
import pathlib
import signal
import time


FIELDS = (
    "utc", "elapsed_s", "host_cpu_total_ticks", "host_cpu_idle_ticks",
    "host_mem_available_kib", "sfu_cpu_ticks", "sfu_rss_pages",
    "net_rx_bytes", "net_rx_packets", "net_rx_errors", "net_rx_drops",
    "net_tx_bytes", "net_tx_packets", "net_tx_errors", "net_tx_drops",
    "udp_in_datagrams", "udp_in_errors", "udp_rcvbuf_errors",
    "udp_sndbuf_errors", "softnet_dropped",
)
STOP = False


def stop(_number, _frame):
    global STOP
    STOP = True


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def default_interface():
    for line in pathlib.Path("/proc/net/route").read_text().splitlines()[1:]:
        parts = line.split()
        if len(parts) > 1 and parts[1] == "00000000":
            return parts[0]
    raise RuntimeError("default_interface_not_found")


def process_stat(pid):
    parts = pathlib.Path("/proc/{}/stat".format(pid)).read_text().rsplit(")", 1)[1].split()
    return int(parts[19]), int(parts[11]) + int(parts[12]), int(parts[21])


def sample(pid, start_ticks, interface, start):
    cpu_values = [int(value) for value in
                  pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:]]
    total = sum(cpu_values)
    idle = cpu_values[3] + cpu_values[4]
    mem = next(int(line.split()[1]) for line in
               pathlib.Path("/proc/meminfo").read_text().splitlines()
               if line.startswith("MemAvailable:"))
    ticks, process_cpu, rss = process_stat(pid)
    if ticks != start_ticks:
        raise RuntimeError("sfu_process_identity_changed")
    net_line = next(line for line in pathlib.Path("/proc/net/dev").read_text().splitlines()
                    if line.strip().startswith(interface + ":"))
    net = [int(value) for value in net_line.split(":", 1)[1].split()]
    snmp = pathlib.Path("/proc/net/snmp").read_text().splitlines()
    udp = {}
    for index in range(len(snmp) - 1):
        if snmp[index].startswith("Udp:") and snmp[index + 1].startswith("Udp:"):
            udp = dict(zip(snmp[index].split()[1:],
                           (int(value) for value in snmp[index + 1].split()[1:])))
            break
    softnet = sum(int(line.split()[1], 16) for line in
                  pathlib.Path("/proc/net/softnet_stat").read_text().splitlines())
    return dict(zip(FIELDS, (
        utc_now(), round(time.monotonic() - start, 3), total, idle, mem,
        process_cpu, rss, net[0], net[1], net[2], net[3],
        net[8], net[9], net[10], net[11], udp.get("InDatagrams"),
        udp.get("InErrors"), udp.get("RcvbufErrors"), udp.get("SndbufErrors"), softnet,
    )))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--duration", type=int, default=600)
    args = parser.parse_args()
    if args.duration < 1 or args.duration > 18000:
        parser.error("duration must be between 1 and 18000 seconds")
    interface = default_interface()
    start_ticks = process_stat(args.pid)[0]
    started = utc_now()
    start = time.monotonic()
    count = 0
    error = None
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        with args.output.open("x", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS)
            writer.writeheader()
            while not STOP and time.monotonic() - start < args.duration:
                writer.writerow(sample(args.pid, start_ticks, interface, start))
                stream.flush()
                count += 1
                time.sleep(max(0, start + count - time.monotonic()))
    except Exception as failure:
        error = type(failure).__name__ + ":" + str(failure)
    status = {"schema": 1, "started_utc": started, "finished_utc": utc_now(),
              "interface": interface, "sfu_pid": args.pid, "samples": count,
              "error": error, "stopped_by_signal": STOP}
    args.output.with_suffix(".status.json").write_text(json.dumps(status) + "\n")
    print(json.dumps(status))
    return 1 if error else 0


if __name__ == "__main__":
    raise SystemExit(main())
