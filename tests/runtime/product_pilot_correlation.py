"""Join explicit test-driver context to native IDs without rewriting either."""
from product_pilot_context import validate_context
import hashlib


def correlate(run, actions, remote, diagnostic_events):
    contexts = {r["sequence"]: r["observer_context"] for r in remote
                if r["event"] == "context.accepted"}
    accepted = {(c["operation_id"], c["phase"]): (seq, validate_context(c, run))
                for seq, c in contexts.items()}
    if len(accepted) != len(contexts):
        raise ValueError("duplicate_context_ack")
    cycles = {}
    for action in actions:
        c = accepted.get((action["operation_id"], action["phase"]))
        if c is None:
            raise ValueError("action_without_observer_fence")
        c = c[1]
        for key in ("run_id", "cycle", "cycle_id", "pid", "process_run_id", "anonymous_session_id", "participant_sha256", "action"):
            if action.get(key) != c[key]:
                raise ValueError("uia_native_observer_identity_mismatch")
        if action["action"] == "join" and action["phase"] == "uia_observed":
            if not c["anonymous_session_id"] or c["cycle"] in cycles:
                raise ValueError("cycle_session_missing_or_duplicate")
            cycles[c["cycle"]] = c
    if not cycles or len({c["anonymous_session_id"] for c in cycles.values()}) != len(cycles):
        raise ValueError("native_session_reused_across_cycles")
    details = []
    for cycle, c in sorted(cycles.items()):
        joined = [r for r in remote if r["event"] == "receiver.participant_joined"
                  and (r.get("observer_context") or {}).get("cycle_id") == c["cycle_id"]
                  and hashlib.sha256((run+":"+r["participant"]).encode()).hexdigest() == c["participant_sha256"]]
        if len(joined) != 1:
            raise ValueError("authenticated_participant_cycle_correlation_missing")
        context = joined[0]["observer_context"]
        if context["action"] != "join" or context["phase"] != "requested":
            raise ValueError("peer_join_outside_acknowledged_operation")
        native = [e for e in diagnostic_events if e.get("anonymous_session_id") == c["anonymous_session_id"]]
        operations = sorted({e["operation_id"] for e in native if "operation_id" in e})
        if not native or not operations or any(e["process_run_id"] != c["process_run_id"] for e in native):
            raise ValueError("native_diagnostic_session_operation_missing")
        details.append(dict(cycle=cycle, cycle_id=c["cycle_id"], process_run_id=c["process_run_id"],
            anonymous_session_id=c["anonymous_session_id"], participant=joined[0]["participant"],
            native_operation_ids=operations, diagnostic_events=len(native)))
    return dict(scope="acknowledged_observer_context_and_run_salted_authenticated_participant; native operation IDs preserved",
                server_native_operation_propagation=False, contexts=len(contexts), cycles=details)
