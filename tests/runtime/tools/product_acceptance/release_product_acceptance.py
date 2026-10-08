"""Issue a fail-closed formal release gate from three unchanged product PILOTs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
from product_gpu_etw import validate_frozen_limits
from product_pilot_scheduler import load_scheduler_policy, validate_scheduler_metadata
from product_pilot_desktop_policy import bind_desktop_input_policy

FULL_MEDIA_GPU_CHECKS = (
    "receiver_audio_rtp_stats", "outbound_audio_pcm_continuity", "inbound_audio_continuity",
    "independent_screen_delivery", "independent_video_buffers_released", "native_cleanup_released",
    "wddm_coverage", "share_stop_resources", "release_wddm_coverage",
    "dxgi_node0_budget_coverage", "dxgi_all_nodes_budget_coverage", "gpu_queue_coverage")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    result=hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda:stream.read(1024*1024), b""): result.update(chunk)
    return result.hexdigest()


def full_media_gpu_gaps(external):
    """Limited-condition PASS cannot authorize the complete B14 GPU scope."""
    gaps = set(external.get("deferred", [])) & {
        "dxgi_local_nonlocal_budget", "dxgi_linked_adapter_nodes_other_than_0", "gpu_queue"}
    cycles = external.get("cycles", [])
    if not cycles:
        gaps.add("full_media_gpu_cycles_missing")
    frozen=external.get("gpu_queue_frozen_limits",{})
    try:
        validate_frozen_limits(frozen.get("policy",{}))
        if not isinstance(frozen.get("sha256"),str) or len(frozen["sha256"])!=64:
            raise ValueError("gpu_queue_frozen_hash_missing")
    except (ValueError,TypeError,KeyError):
        gaps.add("gpu_queue_frozen_limits")
    for cycle in cycles:
        checks = cycle.get("checks", {})
        for name in FULL_MEDIA_GPU_CHECKS:
            if checks.get(name) != "PASS":
                gaps.add(name)
        if checks.get("gpu_queue_coverage")=="PASS":
            proof=cycle.get("details",{}).get("gpu_queue_coverage",{})
            if not isinstance(proof,dict) or proof.get("policy_sha256")!=frozen.get("sha256"):
                gaps.add("gpu_queue_frozen_limits")
        backend = cycle.get("details", {}).get("backend_observed")
        if not isinstance(backend, list) or not backend or not set(backend) <= {"dxgi", "wgc", "gdi"}:
            gaps.add("capture_backend_unproven")
        elif "wgc" in backend:
            if "wgc_handle_ownership" in external.get("deferred", []) or checks.get("wgc_ownership_released") != "PASS":
                gaps.add("wgc_handle_ownership")
    return sorted(gaps)


def frozen_scheduler_release(root, plan, external, inputs):
    policy=validate_scheduler_metadata(plan['collector_scheduler_policy'])
    if load_scheduler_policy(root/'collector-scheduler-policy.json') != policy:
        raise ValueError('collector_scheduler_policy_snapshot_mismatch')
    files=[(p,h) for p,h in inputs.items() if Path(p).name=='product_pilot_scheduler_policy.json']
    if len(files)!=1 or files[0][1]!=policy['sha256']:
        raise ValueError('collector_scheduler_policy_execution_identity_missing')
    if (external.get('final_checks',{}).get('collector_scheduler_policy_complete') is not True
            or external.get('collector_scheduler_policy',{}).get('passed') is not True
            or external['collector_scheduler_policy'].get('policy_sha256')!=policy['sha256']):
        raise ValueError('collector_scheduler_policy_not_proven')
    return policy


def validate_current_source(validation, require_full_media_gpu=False, desktop_input_policy=None):
    """The same read-only validation is used before PILOTs and when issuing READY."""
    if not isinstance(validation,dict):
        raise ValueError("current_source_validation_invalid")
    if desktop_input_policy is not None:
        bind_desktop_input_policy({"desktop_input_policy":desktop_input_policy},validation)
    if validation.get("verdict")!="PASS" or validation.get("historical_crash_regression_closed") is not True:
        raise ValueError("focused_or_crash_regression_not_closed")
    if require_full_media_gpu and (validation.get('diagnostic_only') is not False
            or validation.get('release_eligible') is not True
            or validation.get('current_source_release_gate') is not True):
        raise ValueError('current_source_validation_not_release_eligible')
    if require_full_media_gpu and validation.get("configuration")!="RelWithDebInfo":
        raise ValueError("current_source_validation_configuration_invalid")
    for key,error in (("source_hashes","validated_source_changed"),
                      ("evidence_hashes","validation_evidence_changed")):
        hashes=validation.get(key)
        if not isinstance(hashes,dict) or not hashes:
            raise ValueError("current_source_validation_hashes_missing:"+key)
        for path,expected in hashes.items():
            if not isinstance(path,str) or not path or not isinstance(expected,str) or re.fullmatch(r"[0-9a-f]{64}",expected) is None:
                raise ValueError("current_source_validation_hash_invalid:"+key)
            if digest(path)!=expected:
                raise ValueError(error+":"+path)


def evaluate(roots, validation, require_full_media_gpu=False):
    if len(roots)<3 or len({p.resolve() for p in roots})!=len(roots):
        raise ValueError("three_distinct_full_pairs_required")
    inputs=None
    load=None
    proofs=[]
    run_ids=set()
    scheduler_policy=None
    input_policy=None
    for root in roots:
        if (root/"diagnostic-debugger.json").exists():
            raise ValueError("diagnostic_run_not_release_eligible:"+str(root))
        # This deny-only marker preserves scoped PASS without granting PILOT credit.
        if (root/"qualification-excluded.json").exists():
            raise ValueError("pilot_qualification_explicitly_excluded:"+str(root))
        pilot, external, plan = (read(root/name) for name in
            ("pilot-review.json","external-review.json","plan.json"))
        run_id=plan.get("run_id")
        if not isinstance(run_id,str) or re.fullmatch(r"[0-9a-f]{32}",run_id) is None:
            raise ValueError("pilot_run_identity_invalid")
        actual_policy=bind_desktop_input_policy(plan,pilot,external,read(root/"uia/uia-result.json"))
        if input_policy is not None and actual_policy!=input_policy:
            raise ValueError("pilot_desktop_input_policy_changed")
        input_policy=actual_policy
        current=read(root/"executed-inputs.json")
        runner=read(root/"runner-exit.json")
        if runner["verdict"]!="EVIDENCE_COMPLETE" or runner["exit_code"]!=0 or runner["run_id"]!=plan["run_id"]:
            raise ValueError("runner_not_complete")
        if inputs is None: inputs=current
        if current != inputs: raise ValueError("pilot_execution_inputs_differ")
        if load is None: load=plan["load"]
        if plan["load"]!=load or plan["mode"]!="Pilot": raise ValueError("pilot_load_changed")
        if plan["cycles"] < 2 or external["cycle_counts"] != {"PASS": plan["cycles"]}:
            raise ValueError("same_process_rejoin_not_verified")
        if pilot["verdict"]!="PILOT_PASS" or external["verdict"]!="PASS_WITH_DEFERRED":
            raise ValueError("pilot_gate_not_passed:"+str(root))
        if pilot["run_id"]!=external["run_id"] or pilot["run_id"]!=plan["run_id"]:
            raise ValueError("review_run_identity_mismatch")
        if run_id in run_ids:
            raise ValueError("pilot_run_identity_reused")
        run_ids.add(run_id)
        if require_full_media_gpu:
            current_policy=frozen_scheduler_release(root,plan,external,current)
            if scheduler_policy is not None and scheduler_policy!=current_policy:
                raise ValueError('pilot_scheduler_policy_changed')
            scheduler_policy=current_policy
            if plan.get("gpu_queue_limits_sha256") != external.get("gpu_queue_frozen_limits",{}).get("sha256"):
                raise ValueError("full_media_gpu_frozen_limits_identity_changed")
            gaps = full_media_gpu_gaps(external)
            if gaps:
                raise ValueError("full_media_gpu_not_closed:" + ",".join(gaps))
        allowed={"receiver_audio_rtp_stats","outbound_audio_pcm_continuity"}
        for name,check in pilot["checks"].items():
            if check["status"]!="PASS" and not (name in allowed and check["status"]=="DEFERRED"):
                raise ValueError("unclosed_pilot_gate:"+name)
        proofs.append(dict(root=str(root.resolve()),run_id=plan["run_id"],
            pilot_review_sha256=digest(root/"pilot-review.json"),
            external_review_sha256=digest(root/"external-review.json"),
            performance=pilot["checks"]["log_performance_complete"]["detail"]))
    validate_current_source(validation,require_full_media_gpu,input_policy)
    for path, expected in inputs.items():
        if digest(path)!=expected: raise ValueError("execution_input_changed:"+path)
    subprocess.run(["git","diff","--check"],check=True,capture_output=True)
    binary=[(p,h) for p,h in inputs.items() if Path(p).name.lower()=="cohavora.exe"]
    if len(binary)!=1: raise ValueError("product_binary_identity_missing")
    inputs.update(validation["source_hashes"])
    inputs[str(Path(__file__).resolve())]=digest(__file__)
    return dict(schema=1,verdict="READY",historical_crash_regression_closed=True,
        desktop_input_policy=input_policy,
        full_media_gpu_ready=require_full_media_gpu,
        collector_scheduler_policy=scheduler_policy,
        product_sha256=binary[0][1],inputs=inputs,load=load,pilots=proofs,
        validation=validation,created_utc=datetime.now(timezone.utc).isoformat(),
        deferred=sorted({d for root in roots for d in read(root/"external-review.json")["deferred"]}))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot",type=Path,action="append")
    parser.add_argument("--validation",type=Path,required=True)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--require-full-media-gpu",action="store_true")
    parser.add_argument("--validate-only",action="store_true")
    parser.add_argument("--desktop-input-policy",choices=("strict","diagnostic"))
    args=parser.parse_args()
    if args.validate_only:
        if args.pilot or args.output:
            parser.error("--validate-only cannot issue a release gate")
        validate_current_source(read(args.validation),args.require_full_media_gpu,args.desktop_input_policy)
        print(json.dumps(dict(verdict="CURRENT_SOURCE_VALIDATION_PASS",release_gate_issued=False)))
    else:
        if not args.pilot or args.output is None:
            parser.error("--pilot and --output are required to issue a release gate")
        gate=evaluate(args.pilot,read(args.validation),args.require_full_media_gpu)
        with args.output.open("x",encoding="utf-8") as stream:
            json.dump(gate,stream,indent=2);stream.write("\n")
        print(json.dumps(dict(verdict=gate["verdict"],pilots=len(gate["pilots"]))))
