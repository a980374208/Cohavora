"""B04 real codec/lifecycle gates; negative SDK failures remain explicit evidence."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import re
import socket
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "screen_capture"))
import invoke_screen_share_quality_probe as service

CASES = {
    "prefer-regression-backup": ("prefer-regression", "cleanup", "av1", "av1", "vp8", "vp8"),
    "simulcast-backup": ("simulcast", "cleanup", "av1", "av1", "av1", "vp8"),
    "regression-backup": ("regression", "cleanup", "av1", "av1", "vp8", "vp8"),
    "manual-republish": ("prefer-regression", "republish", "av1", "av1", "vp8", "vp8"),
    "signal-reconnect": ("prefer-regression", "signal-reconnect", "av1", "av1", "vp8", "vp8"),
    "full-reconnect": ("prefer-regression", "full-reconnect", "av1", "av1", "vp8", "vp8"),
    "simulcast-signal-reconnect": ("simulcast", "signal-reconnect", "av1", "av1", "av1", "vp8"),
    "simulcast-full-reconnect": ("simulcast", "full-reconnect", "av1", "av1", "av1", "vp8"),
    "regression-signal-reconnect": ("regression", "signal-reconnect", "av1", "av1", "vp8", "vp8"),
    "regression-full-reconnect": ("regression", "full-reconnect", "av1", "av1", "vp8", "vp8"),
    "disabled-av1": ("prefer-regression", "cleanup", "av1", "vp8", "vp8", "none"),
    "no-video-intersection": ("prefer-regression", "negative", "vp8", "vp8", "none", "none"),
    "allowlist-full-reconnect": ("prefer-regression", "server-reconnect", "av1", "vp8", "vp8", "vp8"),
}


def snapshot(room=None):
    header = "SERVER_CONTAINER=" + repr(service.SERVER_CONTAINER) + "\nSERVER_PORT=" + str(service.SERVER_PORT)
    return service.remote_python(header + "\nROOM=" + repr(room) + "\n" + service.REMOTE_AUTH + """
import subprocess,urllib.request,urllib.error
path=pathlib.Path(CONFIG_PATH)
config=yaml.safe_load(path.read_text())
state=dict(config_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),room=config.get('room',{}),
    video=config.get('video',{}),
    image=subprocess.check_output(['docker','inspect','--format','{{.Image}}',SERVER_CONTAINER],text=True).strip(),
    binary=subprocess.check_output(['docker','exec',SERVER_CONTAINER,'sha256sum','/livekit-server'],text=True).split()[0],
    health_status=urllib.request.urlopen('http://127.0.0.1:'+str(SERVER_PORT),timeout=5).status)
if ROOM:
    request=urllib.request.Request('http://127.0.0.1:'+str(SERVER_PORT)+'/twirp/livekit.RoomService/ListParticipants',
        data=json.dumps(dict(room=ROOM)).encode(),headers={'Authorization':'Bearer '+token('b04-admin',dict(roomAdmin=True,room=ROOM)),
        'Content-Type':'application/json'})
    try:participants=json.load(urllib.request.urlopen(request,timeout=5)).get('participants',[])
    except urllib.error.HTTPError as error:
        if error.code==404:participants=[]
        else:raise
    state['participants_remaining']=len(participants)
    state['publications_remaining']=sum(len(p.get('tracks',[])) for p in participants)
print(json.dumps(state))
""")


def clock_port():
    for _ in range(40):
        port = random.SystemRandom().randint(20000, 59000)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as first, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as second:
            try:
                first.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                second.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                first.bind(("127.0.0.1", port))
                second.bind(("127.0.0.1", port + 1))
                return port
            except OSError:
                pass
    raise RuntimeError("clock_port_unavailable")


def fields(lines, prefix):
    matched = [line for line in lines if line.startswith(prefix)]
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", matched[-1])) if matched else {}


def number(values, name):
    try:
        return float(values[name])
    except (KeyError, ValueError):
        return None


def run_case(case, root, binary_identity, isolated):
    directory = root / case
    directory.mkdir()
    room = "b04-" + uuid.uuid4().hex
    policy, lifecycle, requested, effective, received, backup = CASES[case]
    external = lifecycle == "server-reconnect"
    before = snapshot()
    codecs = {c.get("mime", "").lower() for c in before["room"].get("enabled_codecs", [])}
    if case == "disabled-av1" and (not isolated or "video/vp8" not in codecs or "video/av1" in codecs):
        raise RuntimeError("disabled_case_requires_real_vp8_only_service")
    if case == "no-video-intersection" and (not isolated or any(c.startswith("video/") for c in codecs)):
        raise RuntimeError("negative_case_requires_real_no_video_service")
    if external and (not isolated or codecs or before["video"].get("codec_regression_threshold") != 1):
        raise RuntimeError("policy_change_requires_task_owned_all_codec_primary_service")
    auth = service.credentials(room)
    executable = Path(binary_identity["binary_path"])
    common = [str(executable), "--url", f"ws://123.56.225.164:{service.SERVER_PORT}",
              "--token-env", "COHAVORA_E2E_RUNTIME_TOKEN", "--session", room, "--phase-id", case,
              "--codec", requested, "--expected-codec", requested if external else effective,
              "--source", "screen" if backup != "none" else "camera", "--simulcast", "true",
              "--backup-codec", backup, "--backup-policy", policy, "--auto-backup", "false",
              "--width", "1280", "--height", "720", "--quality", "high", "--probes", "10",
              "--shared-clock-ground-truth", "true", "--clock-loopback-port", str(clock_port()),
              "--lifecycle-probe", lifecycle]
    if backup == "none" and requested == "av1":
        common += ["--scalability-mode", "L1T1"]
    if external:
        common += ["--expected-recovery-codec", effective]
    inputs = ["tests/runtime/probes/test_e2e_media_runtime.cpp", str(Path(__file__)),
              "src/rtc/webrtc_manager.cpp", "src/core/room.cpp", "src/core/room.h",
              "src/core/participant.cpp", "src/core/local_video_track.cpp", "src/signal/signal_client.cpp"]
    manifest = dict(case=case, run_id=room, status="RUNNING", configuration="RelWithDebInfo",
                    started_utc=datetime.now(timezone.utc).isoformat(),
                    head=subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                    requested=requested, effective=effective, expected_receiver=received,
                    lifecycle=lifecycle, backup=backup, policy=policy, binary=binary_identity,
                    command=common, server_before=before,
                    clock_transport="same_host_loopback_udp", media_transport="livekit_sfu",
                    input_sha256={p: hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in inputs})
    manifest_path = directory / "result.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    processes = []
    streams = []
    codes = {}
    try:
        for role, remote in [("receiver", "publisher"), ("publisher", "receiver")]:
            stream = (directory / (role + ".log")).open("w", encoding="utf-8")
            streams.append(stream)
            environment = os.environ.copy()
            environment.update(COHAVORA_E2E_RUNTIME_TOKEN=auth[role], LIVEKIT_TEST_ALLOW_INSECURE="1", RUST_LOG="off")
            if hashlib.sha256(executable.read_bytes()).hexdigest() != binary_identity["binary_sha256"]:
                raise RuntimeError("binary_changed")
            process = subprocess.Popen(common + ["--role", role, "--local-peer", role, "--remote-peer", remote],
                                       env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            processes.append((role, process))
            if role == "receiver":
                time.sleep(.75)
        if external:
            ready_deadline = time.monotonic() + 20
            while "E2E_LIFECYCLE_READY event=server_reconnect" not in (directory / "publisher.log").read_text(encoding="utf-8"):
                if process.poll() is not None or time.monotonic() >= ready_deadline:
                    raise RuntimeError("publisher_not_ready_for_policy_change")
                time.sleep(.1)
            command = [sys.executable, "-B", str(Path(__file__).with_name("prepare_b_acceptance_server.py")),
                       "--profile", "primary", "--codec-profile", "vp8-only",
                       "--output", str(directory / "server-policy-change")]
            completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=60, check=True)
            manifest["server_policy_change"] = json.loads(completed.stdout)
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        deadline = time.monotonic() + 140
        for role, process in processes:
            codes[role] = process.wait(timeout=max(1, deadline - time.monotonic()))
    except Exception as error:
        manifest["failure_type"] = type(error).__name__
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    finally:
        for role, process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            codes[role] = process.returncode
        for stream in streams:
            stream.close()
    publisher = (directory / "publisher.log").read_text(encoding="utf-8", errors="replace").splitlines()
    receiver = (directory / "receiver.log").read_text(encoding="utf-8", errors="replace").splitlines()
    p = fields(publisher, "E2E_SUMMARY role=publisher ")
    r = fields(receiver, "E2E_SUMMARY role=receiver ")
    objects = {values["stage"]: values for line in publisher if line.startswith("E2E_OBJECTS ")
               for values in [fields([line], "E2E_OBJECTS ")]}
    native_policy = fields(publisher, "E2E_SERVER_POLICY ")
    if lifecycle == "negative":
        negative = fields(publisher, "E2E_NEGATIVE rejected=")
        rejection = fields(publisher, "E2E_NEGATIVE operation_error=")
        initial, after = objects.get("negative_baseline", {}), objects.get("negative_after", {})
        checks = dict(real_policy=native_policy.get("video_codecs") == "none",
                      real_sdk_rejection=negative.get("rejected") == "true" and codes.get("publisher") == 1
                          and rejection.get("code") == "10" and rejection.get("operation") == "1"
                          and rejection.get("stage") == "resolve_publish_plan",
                      receiver_connected=r.get("connection_events") == "1",
                      clean=negative.get("clean") == "true" and after.get("senders_with_track") == "0"
                      and after.get("local_publications") == "0",
                      graph_unchanged=bool(initial) and all(initial.get(k) == after.get(k) for k in
                          ["senders_with_track", "senders_without_track", "transceivers"]),
                      no_remote_decode=r.get("decoded_frames") == "0")
    else:
        uncertainty, clock_max = number(p, "clock_uncertainty_p95_us"), number(p, "clock_uncertainty_max_us")
        errors = [number(p, "e2e02_error_p95_us"), number(p, "e2e03_error_p95_us")]
        after = objects.get("final_cleanup", {})
        checks = dict(processes=codes.get("publisher") == 0 and codes.get("receiver") == 0,
                      requested_effective=p.get("requested_codec") == requested and p.get("effective_codec") == effective,
                      actual_sender=effective in p.get("observed_codecs", "").split(",")
                          and received in p.get("observed_codecs", "").split(","),
                      actual_decode=r.get("decoded_receiver_codecs") == received and (number(r, "decoded_frames") or 0) > 0,
                      marker=(number(p, "marker_success_rate") or 0) >= .95,
                      marker_context=r.get("marker_invalid_context") == "0",
                      clock=uncertainty is not None and uncertainty <= 10000 and p.get("clock_valid_measurements") == "10",
                      mapped_error=clock_max is not None and all(e is not None and e <= clock_max for e in errors),
                      dimensions=all(p.get(k) == v for k, v in dict(received_width_min="1280", received_width_max="1280",
                          received_height_min="720", received_height_max="720").items()),
                      cleanup=after.get("senders_with_track") == "0" and after.get("local_publications") == "0",
                      manual_republish=p.get("manual_republish_events") == ("1" if lifecycle == "republish" else "0"),
                      reconnect=p.get("reconnected_events") == ("1" if "reconnect" in lifecycle else "0")
                          and p.get("republish_events") == ("1" if lifecycle in ("full-reconnect", "server-reconnect") else "0"))
        if case == "disabled-av1":
            checks["actual_disabled_policy"] = native_policy.get("video_codecs") == "vp8" and p.get("fallback_reason") != "none"
        if external:
            prior_p = fields(publisher, "E2E_BEFORE_RECOVERY role=publisher ")
            prior_r = fields(receiver, "E2E_BEFORE_RECOVERY role=receiver ")
            recovered = fields(publisher, "E2E_RECOVERY_PLAN ")
            checks["initial_av1_media"] = prior_p.get("effective_codec") == "av1" and "av1" in prior_p.get("observed_codecs", "").split(",") \
                and prior_r.get("decoded_receiver_codecs") == "av1" and (number(prior_r, "decoded_frames") or 0) > 0
            checks["native_policy_recomputed"] = recovered.get("requested_codec") == "av1" \
                and recovered.get("effective_codec") == "vp8" and recovered.get("requested_track_codec") == "av1" \
                and p.get("observed_codecs") == "vp8" \
                and recovered.get("sid_changed") == "true" and p.get("fallback_reason") != "none"
            manifest.update(before_recovery_publisher=prior_p, before_recovery_receiver=prior_r, recovery_plan=recovered)
    try:
        after_server = snapshot(room)
    except Exception as error:
        after_server = dict(snapshot_error=type(error).__name__)
    checks["service_health"] = after_server.get("health_status") == 200
    checks["service_fingerprint"] = all(before[k] == after_server.get(k) for k in
        (["image", "binary"] if external else ["config_sha256", "image", "binary"]))
    if external:
        changed = manifest.get("server_policy_change", {})
        final_codecs = {c.get("mime", "").lower() for c in after_server.get("room", {}).get("enabled_codecs", [])}
        checks["planned_service_policy_change"] = changed.get("shared_service_unchanged") is True \
            and changed.get("config_sha256") == after_server.get("config_sha256") != before["config_sha256"] \
            and "video/vp8" in final_codecs and "video/av1" not in final_codecs
    checks["room_cleanup"] = after_server.get("participants_remaining") == 0 and after_server.get("publications_remaining") == 0
    manifest.update(status="PASS" if all(checks.values()) else "FAIL", checks=checks, exit_codes=codes,
                    publisher_summary=p, receiver_summary=r, objects=objects, native_policy=native_policy,
                    server_after=after_server, finished_utc=datetime.now(timezone.utc).isoformat())
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(dict(case=case, status=manifest["status"], failed_checks=[k for k, value in checks.items() if not value])), flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--isolated-server", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.isolated_server:
        service.CONFIG_PATH = "/root/livekit-b-acceptance-20261001/livekit.yaml"
        service.SERVER_PORT = 17980
        service.SERVER_CONTAINER = "cohavora-b-acceptance-20261001"
    command = ["pwsh", "-NoProfile", "-File", "tests/runtime/tools/diagnostics/verify_runtime_binary.ps1",
               "-Executable", "build-debug/RelWithDebInfo/test_e2e_media_runtime.exe",
               "-ExpectedExecutableName", "test_e2e_media_runtime.exe", "-Configuration", "RelWithDebInfo"]
    binary = json.loads(subprocess.check_output(command, text=True, encoding="utf-8"))
    selected = args.case or [case for case in CASES if case not in ("disabled-av1", "no-video-intersection", "allowlist-full-reconnect")]
    results = []
    for case in selected:
        results.append(run_case(case, args.output, binary, args.isolated_server))
        (args.output / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        if results[-1]["status"] != "PASS":
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
