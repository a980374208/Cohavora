"""Keep colocated test peers local without changing the shared SFU or NIC gate.

Only sockets created by the collector child inherit the run-specific net_cls
class. Exact OUTPUT DNAT rules map the SFU's advertised address/RTC ports to
its own private interface. No INPUT/FORWARD/filter rules or routes are changed.
"""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import signal
import subprocess
from product_pilot_scheduler import (SchedulingStatsLease, configure_receiver_priority, validate_receiver_priority,
    validate_no_realtime, realtime_child_command, restrict_realtime_limit, load_scheduler_policy)


class LocalSfuRoute:
    def __init__(self, target, run_id, result):
        if not re.fullmatch(r"[a-f0-9]{32}", run_id):
            raise ValueError("invalid_route_run_id")
        route = target["collector_media_route"]
        self.public = str(ipaddress.IPv4Address(route["public_ip"]))
        self.private = str(ipaddress.IPv4Address(route["private_ip"]))
        if not ipaddress.ip_address(self.private).is_private or self.public == self.private:
            raise ValueError("invalid_local_sfu_addresses")
        self.ports = [("udp", route["udp_port"]), ("tcp", route["tcp_port"])]
        if any(type(p) is not int or not 1 <= p <= 65535 for _, p in self.ports):
            raise ValueError("invalid_local_sfu_port")
        self.classid = 0xb1400000 | int(run_id[:4], 16)
        self.mount = Path("/sys/fs/cgroup/net_cls,net_prio")
        self.group = self.mount / ("cohavora-b14-" + run_id)
        self.installed = []
        self.created = False
        self.result = result
        self.receiver_nice = 0
        self.timing_diagnostic = False
        self.no_realtime = False
        self.proof = dict(schema=1, run_id=run_id, status="RUNNING",
            scope="collector child only; advertised SFU RTC ports to local private interface",
            classid=self.classid, ports=dict(self.ports),
            started_utc=datetime.now(timezone.utc).isoformat())

    def save(self):
        temp = self.result.with_suffix(".tmp")
        temp.write_text(json.dumps(self.proof, indent=2) + "\n")
        temp.replace(self.result)

    def rule(self, protocol, port):
        return ["-p", protocol, "-d", self.public, "--dport", str(port),
                "-m", "cgroup", "--cgroup", str(self.classid),
                "-m", "comment", "--comment", self.group.name,
                "-j", "DNAT", "--to-destination", f"{self.private}:{port}"]

    def command(self, action, rule):
        return ["iptables", "-w", "5", "-t", "nat", action, "OUTPUT", *rule]

    def __enter__(self):
        try:
            for path in self.mount.rglob("net_cls.classid"):
                if int(path.read_text().strip(), 0) == self.classid:
                    raise ValueError("route_classid_collision")
            self.group.mkdir()
            self.created = True
            (self.group / "net_cls.classid").write_text(str(self.classid))
            for protocol, port in self.ports:
                rule = self.rule(protocol, port)
                subprocess.run(self.command("-I", rule), check=True, capture_output=True)
                self.installed.append(rule)
            self.save()
            return self
        except BaseException:
            self.close()
            raise

    def attach_child(self):
        # Called in the single-threaded wrapper's child before exec/SDK imports.
        (self.group / "cgroup.procs").write_text(str(__import__("os").getpid()))
        configure_receiver_priority(self.receiver_nice, self.timing_diagnostic)
        if self.no_realtime:
            restrict_realtime_limit()

    def close(self):
        errors = []
        for rule in reversed(self.installed):
            result = subprocess.run(self.command("-D", rule), capture_output=True)
            if result.returncode:
                errors.append("route_rule_removal_failed")
        if self.created:
            try:
                self.group.rmdir()
            except OSError:
                errors.append("route_cgroup_removal_failed")
        self.proof["cleanup_complete"] = not errors
        if errors:
            self.proof["cleanup_errors"] = errors
            self.proof["status"] = "FAILED"
        self.proof["finished_utc"] = datetime.now(timezone.utc).isoformat()
        self.save()
        if errors:
            raise RuntimeError("local_sfu_route_cleanup_failed")

    def __exit__(self, kind, value, traceback):
        if kind is not None:
            self.proof["status"] = "FAILED"
            self.proof["error_type"] = kind.__name__
        self.close()

    def counters(self):
        text = subprocess.check_output(["iptables", "-w", "5", "-t", "nat",
                                       "-L", "OUTPUT", "-n", "-v", "-x"], text=True)
        lines = [line.split() for line in text.splitlines() if self.group.name in line]
        if len(lines) != len(self.installed):
            raise ValueError("local_sfu_route_rule_missing")
        return sum(int(line[0]) for line in lines)


def main(args):
    target = json.loads(args.target_config.read_text())
    root = Path(target["remote_root"])
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if (command[:2] != [str(root / "venv/bin/python"), str(root / "product_pilot_remote.py")]
            or "--run-id" not in command
            or command[command.index("--run-id")+1] != args.run_id):
        raise ValueError("unexpected_collector_child")
    import yaml
    raw = Path(target["livekit_config"]).read_bytes()
    config = yaml.safe_load(raw)
    expected = target["collector_media_route"]
    rtc = config["rtc"]
    if (rtc["node_ip"] != expected["public_ip"] or
            rtc["udp_port"] != expected["udp_port"] or rtc["tcp_port"] != expected["tcp_port"]
            or hashlib.sha256(raw).hexdigest() != expected["server_config_sha256"]):
        raise ValueError("local_sfu_configuration_changed")
    route_info = json.loads(subprocess.check_output(
        ["ip", "-json", "route", "get", expected["private_ip"]]))
    if not route_info or route_info[0].get("type") != "local" or route_info[0].get("dev") != "lo":
        raise ValueError("private_sfu_is_not_local")
    with LocalSfuRoute(target, args.run_id, args.result) as route:
        route.proof["server_config_sha256"] = hashlib.sha256(raw).hexdigest()
        route.proof["private_route_device"] = "lo"
        timing = "--timing-diagnostic" in command
        if timing and ("--diagnostic" not in command or "--formal" in command):
            raise ValueError("scheduler_accounting_requires_diagnostic_child")
        requested = validate_receiver_priority(args.diagnostic_receiver_nice, timing)
        no_realtime = validate_no_realtime(getattr(args,'diagnostic_no_realtime',False),timing,requested)
        if ('--diagnostic-no-realtime' in command) != no_realtime:
            raise ValueError('realtime_restriction_child_input_mismatch')
        policy = load_scheduler_policy(getattr(args, 'scheduler_policy', None), timing or no_realtime, requested)
        if ('--scheduler-policy' in command) != (policy is not None):
            raise ValueError('scheduler_policy_child_input_mismatch')
        if policy is not None:
            child_path = Path(command[command.index('--scheduler-policy') + 1])
            if child_path.resolve() != args.scheduler_policy.resolve():
                raise ValueError('scheduler_policy_child_path_mismatch')
            route.proof['scheduler_policy'] = policy
            # This executes only in the child; no shared scheduling/sysctl mutation.
            route.receiver_nice, route.timing_diagnostic = 0, True
        route.no_realtime = no_realtime or policy is not None
        if no_realtime:
            route.proof['realtime_restriction'] = dict(diagnostic_only=True,release_eligible=False,
                scope='collector child and its publisher descendants only',cap_sys_nice='REMOVED_AFTER_EXEC',
                cap_sys_resource='REMOVED_AFTER_EXEC',rtprio_limit=[0,0])
        if timing:
            if ("--diagnostic-receiver-nice" not in command or
                    int(command[command.index("--diagnostic-receiver-nice")+1]) != requested):
                raise ValueError("receiver_priority_input_mismatch")
            route.receiver_nice, route.timing_diagnostic = requested, True
            route.proof["receiver_scheduler"] = dict(nice=requested, policy="SCHED_OTHER", diagnostic_only=True)
        timeout = int(command[command.index("--seconds")+1])+30 if timing else None
        if timing and not 30 < timeout <= 3630:
            raise ValueError("diagnostic_collector_watchdog_invalid")
        lease = SchedulingStatsLease(route.proof) if timing else nullcontext()
        with lease:
            child = subprocess.Popen(realtime_child_command(command) if route.no_realtime else command,
                                     preexec_fn=route.attach_child)
            try:
                for sig in (signal.SIGTERM, signal.SIGINT):
                    signal.signal(sig, lambda signal_number, frame: child.send_signal(signal_number))
                if timing:
                    route.proof["scheduler_statistics"]["maximum_child_seconds"] = timeout
                    route.save()
                code = child.wait(timeout=timeout)
            except BaseException as error:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                if isinstance(error, subprocess.TimeoutExpired):
                    raise RuntimeError("diagnostic_collector_watchdog_timeout") from error
                raise
        route.proof["child_exit_code"] = code
        route.proof["matched_connections"] = route.counters()
        route.proof["status"] = "COMPLETE" if code == 0 and route.proof["matched_connections"] > 0 else "FAILED"
    return 0 if route.proof["status"] == "COMPLETE" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-config", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--diagnostic-receiver-nice", type=int, default=0)
    parser.add_argument('--diagnostic-no-realtime',action='store_true')
    parser.add_argument('--scheduler-policy', type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    raise SystemExit(main(parser.parse_args()))
