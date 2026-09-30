"""Issue a fail-closed formal release gate from three unchanged product PILOTs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    result=hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda:stream.read(1024*1024), b""): result.update(chunk)
    return result.hexdigest()


def evaluate(roots, validation):
    if len(roots)<3 or len({p.resolve() for p in roots})!=len(roots):
        raise ValueError("three_distinct_full_pairs_required")
    inputs=None
    load=None
    proofs=[]
    for root in roots:
        if (root/"diagnostic-debugger.json").exists():
            raise ValueError("diagnostic_run_not_release_eligible:"+str(root))
        pilot, external, plan = (read(root/name) for name in
            ("pilot-review.json","external-review.json","plan.json"))
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
        allowed={"receiver_audio_rtp_stats","outbound_audio_pcm_continuity"}
        for name,check in pilot["checks"].items():
            if check["status"]!="PASS" and not (name in allowed and check["status"]=="DEFERRED"):
                raise ValueError("unclosed_pilot_gate:"+name)
        proofs.append(dict(root=str(root.resolve()),run_id=plan["run_id"],
            pilot_review_sha256=digest(root/"pilot-review.json"),
            external_review_sha256=digest(root/"external-review.json"),
            performance=pilot["checks"]["log_performance_complete"]["detail"]))
    for path, expected in inputs.items():
        if digest(path)!=expected: raise ValueError("execution_input_changed:"+path)
    if validation["verdict"]!="PASS" or not validation["historical_crash_regression_closed"]:
        raise ValueError("focused_or_crash_regression_not_closed")
    for path,expected in validation["source_hashes"].items():
        if digest(path)!=expected: raise ValueError("validated_source_changed:"+path)
    for path,expected in validation["evidence_hashes"].items():
        if digest(path)!=expected: raise ValueError("validation_evidence_changed:"+path)
    subprocess.run(["git","diff","--check"],check=True,capture_output=True)
    binary=[(p,h) for p,h in inputs.items() if Path(p).name.lower()=="cohavora.exe"]
    if len(binary)!=1: raise ValueError("product_binary_identity_missing")
    inputs.update(validation["source_hashes"])
    inputs[str(Path(__file__).resolve())]=digest(__file__)
    return dict(schema=1,verdict="READY",historical_crash_regression_closed=True,
        product_sha256=binary[0][1],inputs=inputs,load=load,pilots=proofs,
        validation=validation,created_utc=datetime.now(timezone.utc).isoformat(),
        deferred=sorted({d for root in roots for d in read(root/"external-review.json")["deferred"]}))


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot",type=Path,action="append",required=True)
    parser.add_argument("--validation",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    gate=evaluate(args.pilot,read(args.validation))
    with args.output.open("x",encoding="utf-8") as stream:
        json.dump(gate,stream,indent=2);stream.write("\n")
    print(json.dumps(dict(verdict=gate["verdict"],pilots=len(gate["pilots"]))))
