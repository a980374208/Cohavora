"""Validate the typed writer envelope without retaining attribute contents.

This detects out-of-contract fields/text in this run, not arbitrary semantic
secrets encoded inside otherwise valid opaque identifiers.
"""
import re

TOP_FIELDS=set("schema_version event_name severity component occurred_at_utc_ms monotonic_us event_sequence process_run_id pid thread_role redaction_version anonymous_session_id operation_id parent_operation_id request_id session_generation room_generation recovery_epoch outcome stage error_code error_layer duration_ms retryable attributes".split())
ATTRIBUTE_FIELDS=set("build_id symbol_identity shutdown_reason outcome drain_result accepted dropped queue_high_water sink_kind reason_code last_committed_sequence chat_kind bytes transfer_kind direction route http_status network_error business_error attempt mode media_kind track_count subscription_state measurement_point expected_endpoints endpoint_id previous_endpoint_id reason from_backend to_backend rtc_error_type status count category window_sample source old_generation current_generation expires_at_utc_ms enabled leave_reason from_mode to_mode".split())
TOP_FIELDS.add("legacy_operation_id")
TOP_FIELDS.add("source_monotonic_us")
# These fields come from the current typed DiagnosticFileSink, not raw SDP or
# arbitrary producer attributes. Keep their numeric/boolean contracts explicit.
UNSIGNED_FIELDS=set("source_monotonic_us binding_epoch begin_us end_us threshold_us device_count round_sequence".split())
BOOLEAN_FIELDS=set("cache_hit after_terminal ice_restart".split())
ATTRIBUTE_FIELDS.update("action boundary description_type pc_role phase signaling_before signaling_after".split())
ATTRIBUTE_FIELDS.update((UNSIGNED_FIELDS - {"source_monotonic_us"}) | BOOLEAN_FIELDS)
TOKENS={"process_run_id","anonymous_session_id","operation_id","parent_operation_id","request_id","legacy_operation_id",
        "build_id","symbol_identity","endpoint_id","previous_endpoint_id"}


def validate_serialized_event(event):
    if not isinstance(event,dict) or set(event)-TOP_FIELDS:
        raise ValueError("privacy_top_level_field")
    attributes=event.get("attributes",{})
    if not isinstance(attributes,dict) or set(attributes)-ATTRIBUTE_FIELDS:
        raise ValueError("privacy_attribute_field")
    for key,value in list(event.items())+list(attributes.items()):
        if key=="attributes": continue
        if key in UNSIGNED_FIELDS and (type(value) is not int or not 0 <= value <= 2**64-1):
            raise ValueError("privacy_unsigned_field:"+key)
        if key in BOOLEAN_FIELDS and type(value) is not bool:
            raise ValueError("privacy_boolean_field:"+key)
        if isinstance(value,str):
            pattern=r"[a-zA-Z0-9_.:\-]{1,160}" if key in TOKENS else r"[a-z][a-z0-9_.\-]{0,95}"
            if not re.fullmatch(pattern,value):
                raise ValueError("privacy_unbounded_or_unstructured_text:"+key)
        elif type(value) not in (int,bool):
            raise ValueError("privacy_unexpected_value_type:"+key)
    return True
