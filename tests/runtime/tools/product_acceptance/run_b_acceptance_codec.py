"""Run the existing B03 gate on ECS; keep credentials out of argv and evidence."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "screen_capture"))
import invoke_screen_share_quality_probe as service
from invoke_screen_share_quality_probe import credentials, remote_python


def idle_peers(room, stop=False):
    source = Path(__file__).with_name("server_idle_peers.py").read_text()
    header = "ROOM=" + repr(room) + "\nSOURCE=" + repr(source) + "\nPORT=" + str(service.SERVER_PORT) + "\n"
    if stop:
        return remote_python(header + """
import json,pathlib,time
root=pathlib.Path('/root/livekit-b-acceptance-20261001/idle')/ROOM
(root/'stop').touch()
for attempt in range(40):
    state=json.loads((root/'state.json').read_text())
    if state.get('terminal')=='CLOSED':break
    time.sleep(.25)
print(json.dumps(state))
""")
    return remote_python(header + service.REMOTE_AUTH + """
import os,pathlib,subprocess,time
root=pathlib.Path('/root/livekit-b-acceptance-20261001/idle')/ROOM
root.mkdir(mode=0o700,parents=True,exist_ok=False)
script=root/'idle_peers.py'
script.write_text(SOURCE)
env=os.environ.copy()
env['PYTHONPATH']='/root/livekit-product-acceptance/collector-python'
env['COHAVORA_IDLE_URL']='ws://127.0.0.1:'+str(PORT)
env['COHAVORA_IDLE_TOKENS']=json.dumps([token('b03-idle-'+str(i),dict(roomJoin=True,room=ROOM,
    canPublish=False,canSubscribe=False,canPublishData=False)) for i in range(3)])
with (root/'worker.log').open('w') as stream:
    subprocess.Popen(['/root/livekit-product-acceptance/venv/bin/python',str(script),
        '--room',ROOM,'--output',str(root)],env=env,stdin=subprocess.DEVNULL,stdout=stream,stderr=stream,
        start_new_session=True)
for attempt in range(60):
    try:state=json.loads((root/'state.json').read_text())
    except (FileNotFoundError,ValueError):state={}
    if state.get('status') in ['READY','FAIL']:break
    time.sleep(.25)
print(json.dumps(state))
""")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case")
    parser.add_argument("--loopback-clock", action="store_true")
    parser.add_argument("--receiver-publish-codec", choices=("vp8", "h264", "vp9", "av1"))
    parser.add_argument("--isolated-server", action="store_true")
    parser.add_argument("--idle-peers", action="store_true")
    args = parser.parse_args()
    if args.isolated_server:
        service.CONFIG_PATH = "/root/livekit-b-acceptance-20261001/livekit.yaml"
        service.SERVER_PORT = 17980
        service.SERVER_CONTAINER = "cohavora-b-acceptance-20261001"
    args.output.mkdir(parents=True, exist_ok=False)
    room = "b03-" + uuid.uuid4().hex
    auth = credentials(room)
    environment = os.environ.copy()
    environment.update(LIVEKIT_L3_TOKEN_S8C_PUBLISHER=auth["publisher"],
                       LIVEKIT_L3_TOKEN_S8C_RECEIVER=auth["receiver"])
    environment["RUST_LOG"] = "off"
    command = ["pwsh", "-NoProfile", "-File",
               "tests/runtime/tools/media/invoke_e2e_media_matrix.ps1",
               "-Url", f"ws://123.56.225.164:{service.SERVER_PORT}", "-PublisherIdentity", "publisher",
               "-ReceiverIdentity", "receiver", "-RunId", room,
               "-EvidenceRoot", str(args.output.resolve()), "-AllowInsecureTransport"]
    if args.case:
        command += ["-CaseId", args.case]
    if args.loopback_clock:
        command += ["-UseLoopbackClock"]
    if args.receiver_publish_codec:
        command += ["-ReceiverPublishCodec", args.receiver_publish_codec]
    inputs = ["tests/runtime/tools/media/invoke_e2e_media_matrix.ps1",
              "tests/runtime/tools/product_acceptance/run_b_acceptance_codec.py",
              "tests/runtime/tools/product_acceptance/server_idle_peers.py",
              "tests/runtime/probes/test_e2e_media_runtime.cpp",
              "src/telemetry/e2e_measurement.cpp",
              "src/core/participant.cpp", "src/core/room.cpp",
              "src/rtc/webrtc_manager.cpp"]
    manifest = {"status": "RUNNING", "run_id": room,
                "started_utc": datetime.now(timezone.utc).isoformat(),
                "configuration": "RelWithDebInfo",
                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "command": command, "case": args.case or "all-nine",
                "server": remote_python("SERVER_CONTAINER=" + repr(service.SERVER_CONTAINER)
                                        + "\nSERVER_PORT=" + str(service.SERVER_PORT) + "\n" + """
import hashlib,json,pathlib,subprocess,yaml,urllib.request
path=pathlib.Path(CONFIG_PATH)
config=yaml.safe_load(path.read_text())
print(json.dumps(dict(config_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
    room=config.get('room',{}),video=config.get('video',{}),rtc={k:v for k,v in config.get('rtc',{}).items()
        if k in ['tcp_port','udp_port','port_range_start','port_range_end','use_external_ip']},
    image=subprocess.check_output(['docker','inspect','--format','{{.Image}}',SERVER_CONTAINER],text=True).strip(),
    binary=subprocess.check_output(['docker','exec',SERVER_CONTAINER,'sha256sum','/livekit-server'],text=True).strip().split()[0],
    health_status=urllib.request.urlopen('http://127.0.0.1:'+str(SERVER_PORT),timeout=5).status)))
"""),
                "input_sha256": {p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
                                  for p in inputs if Path(p).is_file()}}
    manifest_path = args.output / "run.json"
    if args.idle_peers:
        manifest["idle_peers"] = idle_peers(room)
        if manifest["idle_peers"].get("status") != "READY" or manifest["idle_peers"].get("count") != 3:
            manifest.update(status="FAIL", failure="idle_fixture_not_ready")
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            raise RuntimeError("idle_fixture_not_ready")
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    result = None
    try:
        with (args.output / "driver.log").open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, env=environment, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=1200,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
    except Exception as error:
        manifest.update(status="FAIL", failure_type=type(error).__name__)
    finally:
        if args.idle_peers:
            manifest["idle_peers_cleanup"] = idle_peers(room, stop=True)
    cleanup_ok = not args.idle_peers or manifest["idle_peers_cleanup"].get("terminal") == "CLOSED"
    exit_code = result.returncode if result is not None else 2
    if not cleanup_ok:
        exit_code = 2
    manifest.update(status="PASS" if exit_code == 0 else "FAIL",
                    exit_code=exit_code,
                    finished_utc=datetime.now(timezone.utc).isoformat())
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({k: manifest[k] for k in ("status", "run_id", "exit_code")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
