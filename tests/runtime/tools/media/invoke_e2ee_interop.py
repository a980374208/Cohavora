"""Bounded E0 fixture run. Credentials only enter child environments, never files/logs.

PASS here is a media observation subgate, never the complete E0/product verdict.
Uses the existing read-only ECS authentication transport and a unique test room.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "screen_capture"))
from invoke_screen_share_quality_probe import credentials

ROOT = Path(__file__).resolve().parents[4]
NATIVE = ROOT / "build-debug/RelWithDebInfo/test_e2ee_room_peer.exe"
FLUTTER = Path(__file__).with_name("flutter_e2ee_peer") / "build/windows/x64/runner/Release/e2ee_flutter_peer.exe"


class InteractiveFixtureProcess:
    """Owned test child on the input desktop; never changes the user's desktop.

    The execution host may use a private desktop where window focusing cannot
    succeed. Credentials remain in the child environment, and all standard
    handles point to NUL exactly as for the ordinary fixture launcher.
    """
    def __init__(self, command, cwd=None, env=None, **unused):
        import win32api, win32con, win32event, win32file, win32process, pywintypes
        self._api, self._event, self._process = win32api, win32event, win32process
        security = pywintypes.SECURITY_ATTRIBUTES()
        security.bInheritHandle = True
        null = win32file.CreateFile("NUL", win32con.GENERIC_READ | win32con.GENERIC_WRITE,
            win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE, security, win32con.OPEN_EXISTING, 0, None)
        startup = win32process.STARTUPINFO()
        startup.lpDesktop = "winsta0\\default"
        startup.dwFlags = win32con.STARTF_USESTDHANDLES
        startup.hStdInput = startup.hStdOutput = startup.hStdError = null
        self.returncode = None
        try:
            self._handle, thread, self.pid, _ = win32process.CreateProcess(None,
                subprocess.list2cmdline(command), None, None, True,
                win32con.CREATE_NO_WINDOW | win32con.CREATE_UNICODE_ENVIRONMENT,
                env, str(cwd) if cwd else None, startup)
            thread.Close()
        finally:
            null.Close()

    def poll(self):
        if self.returncode is None and self._event.WaitForSingleObject(self._handle, 0) == 0:
            self.returncode = self._process.GetExitCodeProcess(self._handle)
            self._handle.Close()
        return self.returncode

    def wait(self, timeout=None):
        if self.poll() is None:
            if self._event.WaitForSingleObject(self._handle, int(timeout * 1000) if timeout is not None else -1) != 0:
                raise subprocess.TimeoutExpired("owned_e2ee_fixture", timeout)
        return self.poll()

    def terminate(self):
        if self.poll() is None: self._process.TerminateProcess(self._handle, 1)


def sha(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def read_events(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def negative_phase(native, flutter):
    from datetime import datetime
    def event_ms(event):
        return event.get("time_ms") or datetime.fromisoformat(event["time_utc"]).timestamp() * 1000
    changes = [event_ms(e) for e in native + flutter if e["event"] == "key_changed"]
    cutoff = min(changes) if changes else float("inf")
    return ([e for e in native if event_ms(e) < cutoff], [e for e in flutter if event_ms(e) < cutoff])


def rpc_checks(native, flutter):
    checks = {}
    for peer, events in [('native', native), ('flutter', flutter)]:
        for name in ('rpc_handler', 'rpc_response'):
            checks[f'{peer}_{name}'] = any(e['event'] == name and e.get('content_valid') is True for e in events)
        checks[f'{peer}_rpc_no_failure'] = not any(e['event'] == 'rpc_failed' for e in events)
    return checks


def product_media_state_checks(native):
    checks = {}
    for direction in ('tx', 'rx'):
        for kind in ('audio', 'video'):
            rows = [e for e in native if e['event'] == 'product_crypto_state' and
                    e.get('direction') == direction and e.get('kind') == kind]
            # KEY_RATCHETED is not authenticated media success. Preserve id type.
            identity = 'track_sid' if direction == 'rx' else 'native_track_id'
            checks[f'product_state_{direction}_{kind}'] = any(
                e.get('state') == 1 and e.get(identity) and
                e.get('binding_generation') == e.get('observed_room_generation') for e in rows)
    return checks


def native_channel_guard_check(native):
    observations = [e for e in native if e['event'] == 'native_channel_guard']
    return bool(observations) and all(e.get('publisher_required') is True and
                                     e.get('subscriber_required') is True for e in observations)


def unbound_channel_checks(native, flutter, codes):
    # This deliberately has no cryptor: the peer sends ordinary plaintext RTP.
    # Positive transport/data evidence is required, so a dead fixture cannot pass.
    base = evaluate(native, flutter, 'off', codes)
    checks = {k: v for k, v in base.items() if 'decoded_' not in k}
    checks['native_required_channel_guard'] = native_channel_guard_check(native)
    checks['native_no_video_delivery'] = not any(e['event'] == 'decode_counts' and e.get('video_frames', 0) > 0 for e in native)
    checks['native_no_crypto_bindings'] = not any(e['event'] == 'crypto_counts' for e in native)
    for kind in ['audio', 'video']:
        rows = [e for e in native if e['event'] == 'guard_inbound_rtp' and e.get('kind') == kind]
        checks[f'plaintext_{kind}_packets_arrived'] = any(e.get('packets_available') and e.get('packets', 0) > 3 for e in rows)
        if kind == 'video':
            valid = [e for e in rows if e.get('frames_available')]
            checks['plaintext_video_not_decoded'] = bool(valid) and all(e['frames'] == 0 for e in valid)
        else:
            valid = [e for e in rows if e.get('samples_available') and e.get('concealed_available')]
            checks['plaintext_audio_not_decoded'] = bool(valid) and all(e['samples'] <= e['concealed'] for e in valid)
    return checks


def relay_marker_checks(native):
    rows = [e for e in native if e['event'] == 'decode_counts' and e.get('video_frames', 0) > 0]
    return {
        'relay_first_decoded_frame_marker': bool(rows) and all(e.get('relay_first_frame_correct') is True for e in rows),
        'relay_marker_content_valid': bool(rows) and all(e.get('relay_marker_invalid', -1) == 0 for e in rows),
        'relay_marker_sequence_progress': len(rows) >= 2 and rows[-1].get('relay_marker_advances', 0) >= 3
            and rows[-1].get('relay_marker_advances', 0) > rows[0].get('relay_marker_advances', 0),
    }


def evaluate(native, flutter, mode, codes):
    checks = {"native_closed": any(e["event"] == "closed" for e in native),
              "flutter_closed": any(e["event"] == "closed" for e in flutter),
              "process_exit": all(code == 0 for code in codes),
              "no_operation_failure": not any(e["event"] in
                {"fatal", "operation_failed", "join_failed", "publish_failed", "close_failed", "ui_error",
                 "sample_failed", "data_send_failed", "stream_send_failed", "stream_failed"}
                for e in native + flutter)}
    for kind, counter in [("video", "video_frames"), ("audio", "audio_samples")]:
        checks[f"native_decoded_{kind}"] = any(e["event"] == "decode_counts" and e.get(counter, 0) > 0 for e in native)
        stat = "framesDecoded" if kind == "video" else "totalSamplesReceived"
        checks[f"flutter_decoded_{kind}"] = any(e["event"] == "rtp" and e.get("direction") == "rx"
            and e.get("kind", "").lower() == kind and
            e.get("counters", {}).get(stat, 0) - (e.get("counters", {}).get("concealedSamples", 0) if kind == "audio" else 0) > 0 for e in flutter)
        if mode == "on":
            for direction in ["tx", "rx"]:
                checks[f"native_crypto_{direction}_{kind}"] = any(e["event"] == "crypto_counts"
                    and e.get("kind") == kind and e.get("direction") == direction and e.get("output", 0) > 0 for e in native)
    if mode == "on":
        for direction in ["tx", "rx"]:
            # Official plugin states are corroborating evidence; its frame counters are unavailable.
            checks[f"flutter_crypto_{direction}"] = any(e["event"] == "crypto_state"
                and e.get("direction") == direction and e.get("state", "").lower() == "kok" for e in flutter)
    for name, events in [("native", native), ("flutter", flutter)]:
        for event in ["data_received", "stream_received"]:
            checks[f"{name}_{event}"] = any(e["event"] == event and e.get("content_valid") is True for e in events)
    return checks


def observed_profile_checks(native, flutter, codec="vp8"):
    """Requested settings are not proof: both peers must observe the selected codec."""
    checks = {}
    for kind, expected in [("audio", "audio/opus"), ("video", "video/" + codec)]:
        for direction in ["tx", "rx"]:
            frames = [e for e in native if e["event"] == "crypto_counts" and
                      e.get("kind") == kind and e.get("direction") == direction and e.get("input", 0) > 0]
            seen = {mime.lower() for e in frames for mime in e.get("observed_codecs", [])}
            checks[f"native_observed_{direction}_{kind}_codec"] = seen == {expected}
            rtp = [e for e in flutter if e["event"] == "rtp" and
                   e.get("kind", "").lower() == kind and e.get("direction") == direction]
            seen = {e.get("observed_codec") for e in rtp if e.get("observed_codec")}
            checks[f"flutter_observed_{direction}_{kind}_codec"] = seen == {expected}
            track_ids = {e.get("track") for e in rtp if e.get("track")}
            checks[f"flutter_crypto_{direction}_{kind}"] = any(e["event"] == "crypto_state" and
                e.get("direction") == direction and e.get("track") in track_ids and
                e.get("state", "").lower() == "kok" for e in flutter)
    return checks


def key_transition_checks(native_events, flutter_events, rounds):
    checks = {}
    for number in range(1, rounds + 1):
        prefix = "" if number == 1 else f"epoch{number}_"
        for name, events in [("native", native_events), ("flutter", flutter_events)]:
            epoch = [e for e in events if e.get("key_epoch") == number]
            checks[f"{prefix}{name}_key_changed"] = any(e["event"] == "key_changed" for e in epoch)
            for event in ["data_received", "stream_received"]:
                checks[f"{prefix}{name}_post_key_{event}"] = sum(e["event"] == event and e.get("content_valid") is True for e in epoch) >= 2
            for kind in ["audio", "video"]:
                for direction in ["tx", "rx"]:
                    series = [e for e in epoch if e["event"] == ("crypto_counts" if name == "native" else "rtp")
                              and e.get("kind", "").lower() == kind and e.get("direction") == direction]
                    counter = "framesDecoded" if kind == "video" else "totalSamplesReceived"
                    if direction == "tx": counter = "framesEncoded" if kind == "video" else "packetsSent"
                    groups = {}
                    for event in series:
                        identity = (event.get("generation"), event.get("binding_generation"), event.get("track"))
                        value = event["output"] if name == "native" else event.get("counters", {}).get(counter, 0) - (
                            event.get("counters", {}).get("concealedSamples", 0) if kind == "audio" and direction == "rx" else 0)
                        groups.setdefault(identity, []).append(value)
                    checks[f"{prefix}{name}_post_key_{direction}_{kind}"] = any(
                        any(later > earlier for earlier, later in zip(values, values[1:]))
                        for values in groups.values())
    return checks



def flutter_reconnect_progress(events):
    checks = {}
    for direction in ('tx', 'rx'):
        for kind in ('audio', 'video'):
            counter = ('framesEncoded' if kind == 'video' else 'packetsSent') if direction == 'tx' else (
                'framesDecoded' if kind == 'video' else 'totalSamplesReceived')
            groups = {}
            for event in events:
                if event['event'] != 'rtp' or event.get('direction') != direction or event.get('kind', '').lower() != kind:
                    continue
                counters = event.get('counters', {})
                value = counters.get(counter, 0) - (counters.get('concealedSamples', 0)
                    if direction == 'rx' and kind == 'audio' else 0)
                identity = (event.get('generation'), event.get('track'), event.get('ssrc'))
                groups.setdefault(identity, []).append(value)
            checks[f'flutter_post_reconnect_{direction}_{kind}'] = any(
                len(values) >= 2 and values[-1] > values[0] for values in groups.values())
    for name in ('data_received', 'stream_received'):
        checks[f'flutter_post_reconnect_{name}'] = any(
            event['event'] == name and event.get('content_valid') is True for event in events)
    return checks


def run(mode, duration, root, debug_native=False, video_source="window", key_action="none", initial_key="present", late_join=0, reconnect="none", failure_tolerance=-1, rejoin=False, ratchet_window=16, focus_source=False, codec="vp8", interactive_desktop=False, key_rounds=1, repaired_flutter=False, channel_guard_only=False, rpc=False):
    flutter_binary = FLUTTER
    backend = {"kind": "official_release_dll"}
    if repaired_flutter:
        flutter_binary = ROOT / "build-e2ee-backend/flutter-repaired/e2ee_flutter_peer.exe"
        locator = ROOT / "build-e2ee-backend/flutter-repaired-current.json"
        if locator.exists():
            directory = json.loads(locator.read_text())["directory"]
            if Path(directory).name != directory or not directory.startswith("flutter-repaired-"):
                raise ValueError("invalid staged backend directory")
            flutter_binary = ROOT / "build-e2ee-backend" / directory / "e2ee_flutter_peer.exe"
        backend = json.loads(flutter_binary.with_name("backend-provenance.json").read_text())
        if (backend["configuration"] != "Release" or
                sha(flutter_binary) != backend["executable_sha256"] or
                sha(flutter_binary.with_name("libwebrtc.dll")) != backend["dll_sha256"] or
                (backend.get("aot_sha256") and sha(flutter_binary.parent / "data/app.so") != backend["aot_sha256"]) or
                (rpc and not backend.get("aot_sha256")) or
                (backend.get("fixture_source_sha256") and sha(Path(__file__).with_name("flutter_e2ee_peer") / "lib/main.dart") != backend["fixture_source_sha256"])):
            raise ValueError("staged Flutter backend fingerprint mismatch")
    run_id = "e2ee-" + uuid.uuid4().hex[:16]
    output = root / (mode + "-" + run_id)
    output.mkdir(parents=True, exist_ok=False)
    cache = (ROOT / 'build-debug/CMakeCache.txt').read_text(encoding='utf-8')
    package_line = next(line for line in cache.splitlines() if line.startswith('COHAVORA_E2EE_BACKEND_PACKAGE_DIR:PATH='))
    package = Path(package_line.split('=', 1)[1])
    profile_path = ROOT / 'build/prepare/e2ee-backend-profile.json'
    profile = json.loads(profile_path.read_text(encoding='utf-8'))
    native_package_inputs = [profile_path, package / 'package.json']
    for name, expected in profile['files'].items():
        path = package / name
        if sha(path) != expected: raise RuntimeError('native_backend_profile_mismatch')
        native_package_inputs.append(path)
    manifest = {"run_id": run_id, "room": run_id, "mode": mode,
                "scope": "native_required_without_hooks_plaintext_peer" if channel_guard_only else "product_media_user_and_stream_paths",
                "native_configuration": "RelWithDebInfo", "flutter_configuration": "Release", "flutter_backend": backend,
                "duration_seconds": duration, "video_source": video_source, "key_action": key_action, "key_rounds": key_rounds,
                "flutter_initial_key": initial_key, "e0_verdict": "NOT_EVALUATED_BY_THIS_RUN",
                "late_join_seconds": late_join, "reconnect": reconnect,
                "failure_tolerance": failure_tolerance,
                "ratchet_window": ratchet_window,
                "focus_synthetic_source": focus_source,
                "requested_video_codec": codec,
                "fixture_desktop": "interactive_default" if interactive_desktop else "inherited",
                "native_same_process_rejoin": rejoin,
                "rpc_requested": rpc, "status": "STARTING", "sha256": {str(p.relative_to(ROOT)): sha(p) for p in
                    [NATIVE, flutter_binary, flutter_binary.parent / "data/app.so", Path(__file__), ROOT / "tests/runtime/probes/test_e2ee_room_peer.cpp",
                     flutter_binary.parent / "flutter_webrtc_plugin.dll", flutter_binary.parent / "libwebrtc.dll",
                     Path(__file__).with_name("flutter_e2ee_peer") / "pubspec.lock",
                     Path(__file__).with_name("flutter_e2ee_peer") / "lib/main.dart",
                     ROOT / "src/rtc/webrtc_manager.cpp", ROOT / "src/core/room.h", ROOT / "src/core/room.cpp",
                     ROOT / "src/core/track.h", ROOT / "src/rpc/rpc_types.h", ROOT / "src/core/participant.cpp", ROOT / "src/signal/signal_client.cpp",
                     ROOT / "src/e2ee/frame_cryptor.h", ROOT / "src/e2ee/frame_cryptor.cpp",
                     ROOT / "src/e2ee/key_provider.h", ROOT / "src/e2ee/key_provider.cpp",
                     ROOT / "CMakeLists.txt", *native_package_inputs] if p.exists()}}
    def save():
        temp = output / "result.tmp"
        temp.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        temp.replace(output / "result.json")
    save()
    children = []
    source = None
    try:
        auth = credentials(run_id)
        source_command = [sys.executable, str(Path(__file__).resolve()), "--source-window"]
        if focus_source: source_command += ["--focus-source", "--source-evidence", str(output / "source.jsonl")]
        launch = InteractiveFixtureProcess if interactive_desktop else subprocess.Popen
        source = launch(source_command,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(2)
        shared = secrets.token_urlsafe(32)
        env = os.environ.copy()
        env.update(LIVEKIT_URL="ws://123.56.225.164:17880", LIVEKIT_TEST_ALLOW_INSECURE="1",
                   E2EE_TEST_KEY=shared, E2EE_MODE=mode, E2EE_RUN_ID=run_id,
                   E2EE_EVIDENCE_DIR=str(output), E2EE_DURATION=str(duration), E2EE_PUBLISH="1",
                   E2EE_CODEC=codec, E2EE_KEY_STATE="present", E2EE_VIDEO_SOURCE=video_source)
        env["E2EE_RPC"] = "1" if rpc else "0"
        env["E2EE_RPC_AT_MS"] = str(int((time.time() + duration - 15) * 1000))
        env["E2EE_FAILURE_TOLERANCE"] = str(failure_tolerance)
        env["E2EE_REQUIRE_NATIVE_GUARD"] = "1"
        env['E2EE_NATIVE_CHANNEL_GUARD_ONLY'] = '1' if channel_guard_only else '0'
        env["E2EE_RATCHET_WINDOW"] = str(ratchet_window)
        env["E2EE_REJOIN"] = "1" if rejoin else "0"
        env["E2EE_RECOVER_INVALID_KEY"] = "1" if initial_key != "present" and failure_tolerance == 0 else "0"
        env.update(E2EE_KEY_ROUNDS=str(key_rounds), E2EE_KEY_ACTION=key_action, E2EE_NEXT_TEST_KEY=secrets.token_urlsafe(32),
                   E2EE_KEY_ACTION_AT_MS=str(int((time.time() + duration / 2) * 1000)))
        env.update(E2EE_RECONNECT=reconnect, E2EE_RECONNECT_AT_MS=str(int((time.time() + duration / 2) * 1000)))
        startup = [(NATIVE, "publisher"), (flutter_binary, "receiver")] if late_join else [(flutter_binary, "receiver"), (NATIVE, "publisher")]
        for binary, identity in startup:
            if late_join and binary == flutter_binary: time.sleep(late_join)
            child_env = dict(env, LIVEKIT_TOKEN=auth[identity])
            if binary == flutter_binary:
                if rejoin: child_env["E2EE_DURATION"] = str(duration * 2 + 10)
                if initial_key == "wrong": child_env["E2EE_TEST_KEY"] = secrets.token_urlsafe(32)
                if initial_key == "missing": child_env["E2EE_KEY_STATE"] = "missing"
            command = [str(binary)]
            if debug_native and binary == NATIVE:
                stack = output / "native-crash-stack.txt"
                cdb = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
                command = [cdb, "-G", "-c", f'sxe -c ".logopen {stack}; .ecxr; kn; .logclose; q" av; g', str(binary)]
            children.append(launch(command, cwd=binary.parent, env=child_env,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW))
        manifest["pids"] = [p.pid for p in children]
        manifest["status"] = "RUNNING"
        save()
        print(json.dumps({"event": "started", "run_id": run_id, "evidence": str(output)}), flush=True)
        deadline = time.monotonic() + duration * (2 if rejoin else 1) + 90
        while any(p.poll() is None for p in children) and time.monotonic() < deadline:
            time.sleep(1)
        for child in children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=15)
                manifest["timed_out"] = True
        manifest["exit_codes"] = [p.returncode for p in children]
        native_events = read_events(output / "native.jsonl")
        flutter_events = read_events(output / "flutter.jsonl")
        checks = evaluate(native_events, flutter_events, mode, manifest["exit_codes"])
        if video_source == "relay":
            checks.update(relay_marker_checks(native_events))
            manifest['visual_content_scope'] = 'Controlled I420 marker through native encode/encrypt -> Flutter decrypt/decode/re-encode/encrypt -> native decrypt/decode, including first returned decoded frame; not a physical-camera image or final display-pixel assertion'
        if channel_guard_only:
            checks = unbound_channel_checks(native_events, flutter_events, manifest['exit_codes'])
        if mode == 'on':
            checks['native_required_channel_guard'] = native_channel_guard_check(native_events)
            checks.update(product_media_state_checks(native_events))
        if mode == "on": checks.update(observed_profile_checks(native_events, flutter_events, codec))
        if rejoin:
            from datetime import datetime
            second = [e for e in native_events if e.get("generation") == 2]
            checks["native_second_room_closed"] = any(e["event"] == "closed" for e in second)
            checks["native_second_room_connected"] = any(e["event"] == "connected" for e in second)
            first_retired = [e["time_ms"] for e in native_events if e.get("generation") == 1 and e["event"] == "closed"]
            checks["no_events_after_first_room_retirement"] = bool(first_retired) and not any(e.get("generation") == 1 and e["time_ms"] > first_retired[0] for e in native_events)
            start = min((e["time_ms"] for e in second if e["event"] == "connected"), default=float("inf"))
            flutter_second = [e for e in flutter_events if datetime.fromisoformat(e["time_utc"]).timestamp() * 1000 > start]
            for name, passed in evaluate(second, flutter_second, mode, manifest["exit_codes"]).items():
                checks["rejoin_" + name] = passed
        if late_join:
            from datetime import datetime
            native_connected = [e["time_ms"] for e in native_events if e["event"] == "connected"]
            flutter_connected = [datetime.fromisoformat(e["time_utc"]).timestamp() * 1000 for e in flutter_events if e["event"] == "connected"]
            checks["late_join_observed"] = bool(native_connected and flutter_connected) and flutter_connected[0] - native_connected[0] >= (late_join - 2) * 1000
        if reconnect != "none":
            from datetime import datetime
            def timestamp(e):
                return e.get("time_ms") or datetime.fromisoformat(e["time_utc"]).timestamp() * 1000
            reconnect_peer = flutter_events if reconnect == "flutter-full" else native_events
            restored = [timestamp(e) for e in reconnect_peer if e["event"] == "reconnected"]
            checks["reconnect_requested"] = any(e["event"] == "reconnect_requested" for e in reconnect_peer)
            checks["reconnected"] = bool(restored)
            after = [e for e in native_events if restored and e["time_ms"] >= restored[0]]
            for kind in ["video", "audio"]:
                for direction in ["tx", "rx"]:
                    series = {}
                    for e in after:
                        if e["event"] == "crypto_counts" and e.get("kind") == kind and e.get("direction") == direction:
                            series.setdefault((e.get("track"), e.get("binding_generation")), []).append(e["output"])
                    checks[f"post_reconnect_{direction}_{kind}"] = any(len(v) >= 2 and v[-1] > v[0] for v in series.values())
            checks["post_reconnect_data"] = any(e["event"] == "data_received" and e.get("content_valid") is True for e in after)
            checks["post_reconnect_stream"] = any(e["event"] == "stream_received" and e.get("content_valid") is True for e in after)
            flutter_after = [e for e in flutter_events if restored and timestamp(e) >= restored[0]]
            checks.update(flutter_reconnect_progress(flutter_after))
            if reconnect == "flutter-full":
                requested = [timestamp(e) for e in flutter_events if e["event"] == "reconnect_requested"]
                # New receiver kOk can precede RoomReconnected. Require state
                # emitted during THIS reconnect, associated with post-resume SID.
                new_crypto = [e for e in flutter_events if e["event"] == "crypto_state" and requested and
                              restored and requested[0] <= timestamp(e) < restored[0]]
                checks.update({"post_full_" + name: value for name, value in
                    observed_profile_checks(after, flutter_after + new_crypto, codec).items()})
                for event in ["data_received", "stream_received"]:
                    checks["flutter_post_full_" + event] = sum(e["event"] == event and e.get("content_valid") is True for e in flutter_after) >= 2
                # Compare within each SID; a replaced track's counter reset is
                # not growth, and an old cumulative total is not new media.
                for kind in ["audio", "video"]:
                    for direction in ["tx", "rx"]:
                        counter = ("framesDecoded" if direction == "rx" else "framesEncoded") if kind == "video" else ("totalSamplesReceived" if direction == "rx" else "packetsSent")
                        series = {}
                        for e in flutter_after:
                            if e["event"] != "rtp" or e.get("kind", "").lower() != kind or e.get("direction") != direction: continue
                            value = e.get("counters", {}).get(counter, 0)
                            if kind == "audio" and direction == "rx": value -= e.get("counters", {}).get("concealedSamples", 0)
                            series.setdefault(e.get("track"), []).append(value)
                        checks[f"flutter_post_full_growth_{direction}_{kind}"] = any(len(v) >= 2 and v[-1] > v[0] for v in series.values())
        if initial_key != "present":
            # Both receivers preinstall slot 1. Receiving a valid slot-1 packet
            # before the LOCAL sender changes is legitimate, not a wrong-key
            # delivery. The negative interval ends at the first sender change.
            before_native, before_flutter = negative_phase(native_events, flutter_events)
            checks["negative_native_video_encrypted_input"] = any(e["event"] == "crypto_counts" and e.get("kind") == "video"
                and e.get("direction") == "tx" and e.get("output", 0) > 0 for e in before_native)
            checks["negative_flutter_video_packets_received"] = any(e["event"] == "rtp" and e.get("kind", "").lower() == "video"
                and e.get("direction") == "rx" and e.get("counters", {}).get("packetsReceived", 0) > 0 for e in before_flutter)
            checks["negative_flutter_video_not_decoded"] = not any(e["event"] == "rtp" and e.get("direction") == "rx"
                and e.get("counters", {}).get("framesDecoded", 0) > 0 for e in before_flutter)
            for name, events in [("native", before_native), ("flutter", before_flutter)]:
                checks[f"negative_{name}_no_user_or_stream_delivery"] = not any(e["event"] in {"data_received", "stream_received"} for e in events)
            incoming_audio = [e for e in before_native if e["event"] == "crypto_counts"
                              and e.get("direction") == "rx" and e.get("kind") == "audio"]
            checks["negative_native_audio_zero_crypto_output"] = bool(incoming_audio) and all(e["output"] == 0 for e in incoming_audio)
            if initial_key == "wrong":
                checks["negative_native_audio_ciphertext_arrived"] = any(e["input"] > 0 for e in incoming_audio)
            if initial_key == "missing":
                # Missing key rejects the send operation. Only this specific
                # pre-recovery platform failure is expected; later errors fail.
                normalized = [e for e in flutter_events if not (e["event"] == "sample_failed"
                    and e.get("key_epoch") == 0 and e.get("type") == "PlatformException")]
                checks["no_operation_failure"] = evaluate(native_events, normalized, mode, manifest["exit_codes"])["no_operation_failure"]
        if key_action != "none":
            checks.update(key_transition_checks(native_events, flutter_events, key_rounds))
        if rpc: checks.update(rpc_checks(native_events, flutter_events))
        manifest["checks"] = checks
        if (output / "native-crash-stack.txt").exists():
            checks["no_native_crash"] = False
        manifest["status"] = "PASS" if all(checks.values()) and not manifest.get("timed_out") else "FAIL"
        manifest["not_covered"] = ["Qt product entry points", "visual/audio quality",
            "per-frame Flutter crypto counts", "performance acceptance", "product chat/whiteboard/RPC functional acceptance"]
        if key_action == "none": manifest["not_covered"].append("key change")
        if reconnect == "none": manifest["not_covered"].append("signal reconnect")
        if reconnect != "native-full": manifest["not_covered"].append("native full reconnect")
        if reconnect != "flutter-full": manifest["not_covered"].append("Flutter full reconnect")
        if not rejoin: manifest["not_covered"].append("same-process leave/rejoin")
        if initial_key == "present": manifest["not_covered"].append("wrong/missing key")
        if video_source == "relay": manifest["not_covered"].append("camera/screen capture")
    except Exception as error:
        manifest.update(status="BLOCKED", error_type=type(error).__name__)
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=15)
        if source is not None and source.poll() is None:
            source.terminate()
            source.wait(timeout=15)
        save()
    print(json.dumps({"event": "finished", "status": manifest["status"], "evidence": str(output),
                      "checks": manifest.get("checks", {})}), flush=True)
    return manifest["status"] == "PASS"


if __name__ == "__main__":
    if "--source-window" in sys.argv:
        import tkinter as tk
        window = tk.Tk()
        window.title("E2EE Synthetic Source")
        window.geometry("640x360+40+40")
        window.attributes("-topmost", True)
        canvas = tk.Canvas(window, width=640, height=360, highlightthickness=0)
        canvas.pack(fill="both", expand=True)
        if "--focus-source" in sys.argv:
            import ctypes
            from ctypes import wintypes
            user32 = ctypes.windll.user32
            user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
            user32.GetAncestor.restype = wintypes.HWND
            user32.GetForegroundWindow.restype = wintypes.HWND
            user32.SetForegroundWindow.argtypes = [wintypes.HWND]
            user32.SetForegroundWindow.restype = wintypes.BOOL
            user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
            user32.AttachThreadInput.restype = wintypes.BOOL
            user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            user32.IsWindow.argtypes = [wintypes.HWND]
            user32.IsWindowVisible.argtypes = [wintypes.HWND]
            user32.OpenInputDesktop.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            user32.OpenInputDesktop.restype = wintypes.HANDLE
            user32.CloseDesktop.argtypes = [wintypes.HANDLE]
            user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int,
                ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
            target_evidence = Path(sys.argv[sys.argv.index("--source-evidence") + 1])
            began = time.monotonic()
            def focus_fixture():
                # Target only this public synthetic fixture, never user windows.
                window.focus_force()
                hwnd = user32.GetAncestor(window.winfo_id(), 2)
                requested = bool(user32.SetForegroundWindow(hwnd))
                # A non-foreground test process may lack activation rights.
                # Temporarily join input queues only to activate OUR window,
                # and detach immediately. No input is sent to the other app.
                attached = False
                foreground_thread = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
                current_thread = ctypes.windll.kernel32.GetCurrentThreadId()
                if not requested and foreground_thread and foreground_thread != current_thread:
                    attached = bool(user32.AttachThreadInput(current_thread, foreground_thread, True))
                    if attached:
                        try:
                            requested = bool(user32.SetForegroundWindow(hwnd))
                        finally:
                            user32.AttachThreadInput(current_thread, foreground_thread, False)
                actual = user32.GetForegroundWindow()
                foreground = actual == hwnd
                owner = wintypes.DWORD()
                user32.GetWindowThreadProcessId(actual, ctypes.byref(owner))
                desktop = user32.OpenInputDesktop(0, False, 1)
                desktop_accessible = bool(desktop)
                desktop_name = "unknown"
                if desktop:
                    name_buffer = ctypes.create_unicode_buffer(256)
                    needed = wintypes.DWORD()
                    if user32.GetUserObjectInformationW(desktop, 2, name_buffer,
                            ctypes.sizeof(name_buffer), ctypes.byref(needed)):
                        value = name_buffer.value.lower()
                        desktop_name = value if value in {"default", "winlogon"} else "other"
                if desktop: user32.CloseDesktop(desktop)
                with target_evidence.open("a", encoding="utf-8") as log:
                    log.write(json.dumps({"event": "fixture_focus", "time_ms": int(time.time()*1000),
                                          "foreground": foreground, "request_succeeded": requested,
                                          "fixture_input_queue_joined": attached,
                                          "window_valid": bool(user32.IsWindow(hwnd)),
                                          "window_visible": bool(user32.IsWindowVisible(hwnd)),
                                          "foreground_available": bool(actual),
                                          "input_desktop_accessible": desktop_accessible,
                                          "input_desktop": desktop_name,
                                          "foreground_is_source_process": owner.value == os.getpid()}) + "\n")
                if time.monotonic() - began < 12: window.after(250, focus_fixture)
            window.after(100, focus_fixture)
        def animate(tick=0):
            canvas.delete("all")
            canvas.configure(bg="#143456")
            x = (tick * 6) % 540
            canvas.create_rectangle(x, 80, x + 100, 250, fill="#51db88")
            canvas.create_text(320, 35, text="Public synthetic E2EE fixture", fill="white", font=("Arial", 20))
            window.after(100, animate, tick + 1)
        animate()
        if '--source-owner-pid' in sys.argv:
            import ctypes
            from ctypes import wintypes
            kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel32.OpenProcess.restype = wintypes.HANDLE
            kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel32.WaitForSingleObject.restype = wintypes.DWORD
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            owner_pid = int(sys.argv[sys.argv.index('--source-owner-pid') + 1])
            owner_handle = kernel32.OpenProcess(0x00100000, False, owner_pid)
            if not owner_handle: raise RuntimeError('source_owner_unavailable')
            def check_owner():
                status = kernel32.WaitForSingleObject(owner_handle, 0)
                if status != 258:
                    kernel32.CloseHandle(owner_handle)
                    window.destroy()
                else:
                    window.after(1000, check_owner)
            window.after(1000, check_owner)
        else:
            window.after(420000, window.destroy)
        window.mainloop()
        sys.exit(0)
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["off", "on"], required=True)
    parser.add_argument("--duration", type=int, default=45)
    parser.add_argument("--debug-native", action="store_true")
    parser.add_argument("--video-source", choices=["window", "relay"], default="window")
    parser.add_argument("--key-action", choices=["none", "replace", "ratchet", "slot"], default="none")
    parser.add_argument("--repaired-flutter", action="store_true")
    parser.add_argument("--rpc", action="store_true")
    parser.add_argument("--channel-guard-only", action="store_true")
    parser.add_argument("--key-rounds", type=int, default=1)
    parser.add_argument("--initial-key", choices=["present", "wrong", "missing"], default="present")
    parser.add_argument("--late-join", type=int, default=0)
    parser.add_argument("--reconnect", choices=["none", "signal", "flutter-full", "native-full"], default="none")
    parser.add_argument("--failure-tolerance", type=int, choices=[-1, 0], default=-1)
    parser.add_argument("--rejoin", action="store_true")
    parser.add_argument("--ratchet-window", type=int, choices=[0, 16], default=16)
    parser.add_argument("--focus-source", action="store_true")
    parser.add_argument("--codec", choices=["vp8", "h264", "vp9"], default="vp8")
    parser.add_argument("--interactive-desktop", action="store_true")
    args = parser.parse_args()
    if args.rpc and (args.duration < 30 or args.channel_guard_only or args.rejoin):
        parser.error("RPC requires duration >= 30 and no channel-guard-only/rejoin")
    if not 10 <= args.duration <= 300:
        parser.error("duration must be 10..300")
    if args.channel_guard_only and (args.mode != 'off' or args.key_action != 'none' or args.reconnect != 'none' or args.rejoin or args.late_join):
        parser.error('channel guard only requires off, no key action, and no reconnect/rejoin/late join')
    if not 1 <= args.key_rounds <= 5:
        parser.error("key rounds must be 1..5")
    if args.key_rounds > 1 and (args.mode != "on" or args.key_action not in ("replace", "ratchet") or args.duration < 20 * args.key_rounds + 20):
        parser.error("multiple key rounds require on + replace/ratchet and duration >= 20*rounds+20")
    if args.initial_key != "present" and (args.mode != "on" or args.key_action != "slot"):
        parser.error("negative key recovery requires --mode on --key-action slot")
    if not 0 <= args.late_join <= args.duration / 3:
        parser.error("late join delay must be 0..duration/3")
    if args.rejoin and (args.key_action != "none" or args.reconnect != "none" or args.late_join):
        parser.error("rejoin is an isolated lifecycle case")
    try:
        ok = run(args.mode, args.duration, ROOT / "docs/analysis/e2ee/evidence/e0-room-20260930", args.debug_native, args.video_source, args.key_action, args.initial_key, args.late_join, args.reconnect, args.failure_tolerance, args.rejoin, args.ratchet_window, args.focus_source, args.codec, args.interactive_desktop, args.key_rounds, args.repaired_flutter, args.channel_guard_only, args.rpc)
    except Exception as error:
        print(json.dumps({"status": "BLOCKED", "error_type": type(error).__name__}))
        ok = False
    sys.exit(0 if ok else 1)
