"""Read bounded, source-bound live sender evidence without exposing raw stdout.

The sender's elapsed clock starts immediately after it writes the task-owned READY
file. Its mtime anchors that clock once to the observer's monotonic clock. RID
matching is correlated publisher evidence; it does not identify receiver RTP RID
or SSRC, which the SFU can rewrite.
"""
from __future__ import annotations

import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import time

import b11_highres_input_freeze as freeze


MAXIMUM_AGE_SECONDS = 3.0
KNOWN_RIDS = frozenset(("q", "h", "f"))
_PREFIX = b"SOURCE_PROBE "
_MAXIMUM_LINE_BYTES = 256 * 1024
_READ_BYTES = 64 * 1024
_COUNTER_MAXIMUM = (1 << 64) - 1
SAFE_ERROR_REASONS = frozenset((
    "publisher_probe_invalid_source", "publisher_probe_invalid_stream",
    "publisher_probe_invalid_binding", "publisher_probe_invalid_ready_anchor",
    "publisher_probe_file_changed", "publisher_probe_invalid_file",
    "publisher_probe_line_too_large", "publisher_probe_invalid_json",
    "publisher_probe_invalid_sample", "publisher_probe_sender_failed",
    "publisher_probe_future_sample", "publisher_probe_elapsed_not_increasing",
    "publisher_probe_capture_counter_regressed", "publisher_probe_closed",
    "publisher_probe_backend_capture_counter_regressed",
    "publisher_probe_invalid_clock", "publisher_probe_future_ready_anchor",
    "publisher_probe_read_failed",
))


def safe_error_reason(error):
    """Return an exact allowlisted code, never a prefix or raw exception text."""
    if type(error) is ValueError and str(error) in SAFE_ERROR_REASONS:
        return str(error)
    return None


def _number(value, *, minimum=0):
    try:
        return (type(value) in (int, float) and math.isfinite(value)
                and value >= minimum)
    except OverflowError:
        return False


def _integer(value, *, minimum=0, maximum=_COUNTER_MAXIMUM):
    return type(value) is int and minimum <= value <= maximum


def _source_binding(source):
    if type(source) is not dict:
        raise ValueError("publisher_probe_invalid_source")
    kind = source.get("scenario")
    description = source.get("source")
    identity = source.get("target_identity")
    layers = source.get("layers")
    expected = freeze.SOURCES.get(kind) if type(kind) is str else None
    fps = expected["source_fps"] if expected else None
    if (fps is None or type(description) is not dict
            or description.get("source") != expected["source"]
            or not _integer(description.get("width")) or description["width"] != expected["width"]
            or not _integer(description.get("height")) or description["height"] != expected["height"]
            or not _integer(description.get("source_fps")) or description["source_fps"] != fps
            or kind.endswith("4k") and any(description.get(name) != expected[name]
                for name in ("capture_backend", "source_scope"))
            or type(identity) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", identity)
            or type(layers) is not dict or not {"low", "high"}.issubset(layers)
            or not set(layers).issubset({"low", "medium", "high"})):
        raise ValueError("publisher_probe_invalid_source")
    mapped = {}
    for name, layer in layers.items():
        if (type(layer) is not dict or type(layer.get("rid")) is not str
                or layer["rid"] not in KNOWN_RIDS or layer["rid"] in mapped
                or not _integer(layer.get("width"), minimum=1, maximum=expected["width"])
                or not _integer(layer.get("height"), minimum=1, maximum=expected["height"])
                or not _number(layer.get("source_fps"), minimum=1)
                or layer["source_fps"] > fps):
            raise ValueError("publisher_probe_invalid_source")
        mapped[layer["rid"]] = name
    return freeze.scenario_kind(kind), fps, hashlib.sha256(identity.encode("utf-8")).hexdigest()[:16], mapped


def _stream(value):
    if type(value) is not dict:
        raise ValueError("publisher_probe_invalid_stream")
    rid = value.get("rid")
    if type(rid) is not str or not re.fullmatch(r"[A-Za-z0-9_-]{0,64}", rid):
        raise ValueError("publisher_probe_invalid_stream")
    if (not _integer(value.get("width"), maximum=16384)
            or not _integer(value.get("height"), maximum=16384)
            or not _number(value.get("fps"))
            or not _integer(value.get("frames_encoded"))):
        raise ValueError("publisher_probe_invalid_stream")
    available = ("width_available", "height_available", "fps_available", "frames_encoded_available")
    if any(type(value.get(name)) is not bool for name in available):
        raise ValueError("publisher_probe_invalid_stream")
    # Unknown RID values cannot become labels or disclose arbitrary stdout data.
    return {"rid": rid if rid in KNOWN_RIDS else "", "width": value["width"],
            "height": value["height"], "fps": value["fps"],
            "frames_encoded": value["frames_encoded"],
            **{name: value[name] for name in available}}


class PublisherProbeSampler:
    """Incrementally consume complete lines; sample(now) takes monotonic seconds.

    Only the most recent valid sample is returned, and only for age <= 3 seconds.
    READY mtime prevents pre-existing old samples being refreshed by reading them.
    Invalid/failure events permanently fail this reader with a safe ValueError.
    """

    def __init__(self, path, source, sid_hash):
        self._kind, self._fps, self._identity_hash, _ = _source_binding(source)
        self._width, self._height = source["source"]["width"], source["source"]["height"]
        self._four_k = source["scenario"].endswith("4k")
        if type(sid_hash) is not str or not re.fullmatch(r"[0-9a-f]{16}", sid_hash):
            raise ValueError("publisher_probe_invalid_binding")
        self._sid_hash = sid_hash
        try:
            ready = Path(source["artifacts"]["publisher_ready"]["path"])
            if not ready.is_absolute():
                raise ValueError
            ready_stat = ready.stat()
            if not stat.S_ISREG(ready_stat.st_mode):
                raise ValueError
            self._ready_wall_seconds = ready_stat.st_mtime
            self._path = Path(path)
        except (KeyError, TypeError, ValueError, OSError):
            raise ValueError("publisher_probe_invalid_ready_anchor") from None
        self._file = None
        self._file_identity = None
        self._offset = 0
        self._pending = b""
        self._discarding_line = False
        self._oversized_probe = False
        self._anchor = None
        self._last_now = None
        self._last_read_finished = None
        self._last_elapsed = None
        self._last_captured = None
        self._last_backend_captured = None
        self._latest = None
        self._closed = False
        self._failure = None

    def close(self):
        if self._file is not None:
            self._file.close()
            self._file = None
        self._pending = b""
        self._closed = True

    def _read_lines(self):
        try:
            path_stat = self._path.stat()
        except FileNotFoundError:
            if self._file is None:
                return
            raise ValueError("publisher_probe_file_changed") from None
        if not stat.S_ISREG(path_stat.st_mode):
            raise ValueError("publisher_probe_invalid_file")
        identity = (path_stat.st_dev, path_stat.st_ino)
        if self._file is None:
            self._file = self._path.open("rb", buffering=0)
            opened_stat = os.fstat(self._file.fileno())
            self._file_identity = (opened_stat.st_dev, opened_stat.st_ino)
            if self._file_identity != identity:
                raise ValueError("publisher_probe_file_changed")
        if identity != self._file_identity or path_stat.st_size < self._offset:
            raise ValueError("publisher_probe_file_changed")
        # Consume only this finite snapshot. Bytes appended during the read are
        # left for the next call, rather than chasing a continuously live file.
        remaining = path_stat.st_size - self._offset
        while remaining:
            chunk = self._file.read(min(_READ_BYTES, remaining))
            if not chunk:
                raise ValueError("publisher_probe_file_changed")
            self._offset += len(chunk)
            remaining -= len(chunk)
            while chunk:
                newline = chunk.find(b"\n")
                if self._discarding_line:
                    if newline < 0:
                        break
                    chunk = chunk[newline + 1:]
                    self._discarding_line = False
                    if self._oversized_probe:
                        raise ValueError("publisher_probe_line_too_large")
                    continue
                if newline < 0:
                    self._pending += chunk
                    if len(self._pending) > _MAXIMUM_LINE_BYTES:
                        self._oversized_probe = self._pending.startswith(_PREFIX)
                        self._discarding_line = True
                        self._pending = b""
                    break
                line = self._pending + chunk[:newline]
                self._pending = b""
                chunk = chunk[newline + 1:]
                if len(line) > _MAXIMUM_LINE_BYTES:
                    if line.startswith(_PREFIX):
                        raise ValueError("publisher_probe_line_too_large")
                    continue
                yield line.removesuffix(b"\r")

    def _accept(self, line, current_elapsed):
        if not line.startswith(_PREFIX):
            return
        try:
            value = json.loads(line[len(_PREFIX):].decode("utf-8"))
        except (ValueError, UnicodeError):
            raise ValueError("publisher_probe_invalid_json") from None
        if type(value) is not dict:
            raise ValueError("publisher_probe_invalid_sample")
        event = value.get("event")
        if event in ("ready", "result"):
            return
        if event == "failure":
            raise ValueError("publisher_probe_sender_failed")
        if (event != "sample" or type(value.get("schema")) is not int or value["schema"] != 1
                or value.get("identity_hash") != self._identity_hash
                or value.get("publication_sid_hash") != self._sid_hash
                or value.get("source_kind") != self._kind
                or type(value.get("source_width")) is not int or value["source_width"] != self._width
                or type(value.get("source_height")) is not int or value["source_height"] != self._height
                or type(value.get("target_fps")) is not int or value["target_fps"] != self._fps
                or not _number(value.get("elapsed_seconds"))
                or not _integer(value.get("captured_frames"))
                or type(value.get("outbound")) is not list or len(value["outbound"]) > 32):
            raise ValueError("publisher_probe_invalid_sample")
        if self._four_k and (value.get("capture_backend") != "wgc_window"
                or value.get("source_scope") != "owned_window_native_fixture"
                or not _integer(value.get("capturer_id")) or value["capturer_id"] != 1
                or not _integer(value.get("capture_size_mismatches")) or value["capture_size_mismatches"] != 0
                or not _integer(value.get("capture_frames"), minimum=1)
                or value["captured_frames"] < 1
                # Windows x64 fixture reads the API count first. The capture
                # wrapper increments its delivery count before calling the API;
                # an in-flight callback means these counters need not be equal.
                or value["capture_frames"] < value["captured_frames"]):
            raise ValueError("publisher_probe_invalid_sample")
        elapsed = value["elapsed_seconds"]
        if elapsed > current_elapsed:
            raise ValueError("publisher_probe_future_sample")
        if self._last_elapsed is not None and elapsed <= self._last_elapsed:
            raise ValueError("publisher_probe_elapsed_not_increasing")
        if self._last_captured is not None and value["captured_frames"] < self._last_captured:
            raise ValueError("publisher_probe_capture_counter_regressed")
        if self._four_k and self._last_backend_captured is not None and value["capture_frames"] < self._last_backend_captured:
            raise ValueError("publisher_probe_backend_capture_counter_regressed")
        outbound = [_stream(item) for item in value["outbound"]]
        self._latest = {"identity_hash": self._identity_hash, "publication_sid_hash": self._sid_hash,
                        "source_kind": self._kind, "elapsed_seconds": elapsed,
                        "captured_frames": value["captured_frames"], "outbound": outbound}
        if self._four_k:
            self._latest.update(capture_backend="wgc_window", source_scope="owned_window_native_fixture",
                capturer_id=1, capture_frames=value["capture_frames"], capture_size_mismatches=0)
        self._last_elapsed = elapsed
        self._last_captured = value["captured_frames"]
        if self._four_k:
            self._last_backend_captured = value["capture_frames"]

    def sample(self, now):
        if self._failure is not None:
            raise ValueError(self._failure)
        if self._closed:
            raise ValueError("publisher_probe_closed")
        try:
            if not _number(now) or (self._last_now is not None and now < self._last_now):
                raise ValueError("publisher_probe_invalid_clock")
            self._last_now = now
            read_started = time.monotonic()
            if (not _number(read_started) or read_started < now
                    or self._last_read_finished is not None and read_started < self._last_read_finished):
                raise ValueError("publisher_probe_invalid_clock")
            if self._anchor is None:
                ready_age = time.time() - self._ready_wall_seconds
                if not _number(ready_age):
                    raise ValueError("publisher_probe_future_ready_anchor")
                # Pair READY's wall time with this call's actual monotonic
                # reading. The caller may have been descheduled after now.
                self._anchor = read_started - ready_age
            lines = list(self._read_lines())
            read_finished = time.monotonic()
            if not _number(read_finished) or read_finished < read_started:
                raise ValueError("publisher_probe_invalid_clock")
            self._last_read_finished = read_finished
            # A complete line may be written after the caller obtained now.
            # Compare against the clock after the bounded read, with no grace
            # period for genuinely future samples or stale evidence.
            current_elapsed = read_finished - self._anchor
            for line in lines:
                self._accept(line, current_elapsed)
            if self._latest is None:
                return None
            age = current_elapsed - self._latest["elapsed_seconds"]
            if not 0 <= age <= MAXIMUM_AGE_SECONDS:
                return None
            return {"age_seconds": age, **copy.deepcopy(self._latest)}
        except (ValueError, OSError) as error:
            reason = safe_error_reason(error) or "publisher_probe_read_failed"
            self._failure = reason
            self.close()
            raise ValueError(reason) from None


def match_layer(track, sample, source):
    """Return one correlated frozen layer, or None for unknown/ambiguous evidence.

    Actual sender dimensions may shrink below the announced maximum. Active
    candidates must exactly match the receiver sink dimensions, and the sole
    candidate's RID is interpreted using the source's frozen per-layer mapping.
    """
    try:
        kind, _, identity_hash, mapped = _source_binding(source)
        if (type(track) is not dict or type(sample) is not dict
                or sample.get("identity_hash") != identity_hash
                or sample.get("source_kind") != kind
                or sample.get("publication_sid_hash") != track.get("sid_hash")
                or type(sample.get("publication_sid_hash")) is not str
                or not re.fullmatch(r"[0-9a-f]{16}", sample["publication_sid_hash"])
                or track.get("source") != source["source"]["source"]
                or track.get("sink_active") is not True
                or track.get("sink_frame_dimensions_available") is not True
                or not _integer(track.get("sink_frame_width"), minimum=1, maximum=source["source"]["width"])
                or not _integer(track.get("sink_frame_height"), minimum=1, maximum=source["source"]["height"])
                or not _number(sample.get("age_seconds"))
                or sample["age_seconds"] > MAXIMUM_AGE_SECONDS
                or type(sample.get("outbound")) is not list):
            return None
        if source["scenario"].endswith("4k") and (sample.get("capture_backend") != "wgc_window"
                or sample.get("source_scope") != "owned_window_native_fixture"
                or not _integer(sample.get("capturer_id")) or sample["capturer_id"] != 1
                or not _integer(sample.get("capture_size_mismatches")) or sample["capture_size_mismatches"] != 0
                or not _integer(sample.get("capture_frames"), minimum=1)
                or not _integer(sample.get("captured_frames"), minimum=1)
                or sample["capture_frames"] < sample["captured_frames"]):
            return None
        dimensions = (track["sink_frame_width"], track["sink_frame_height"])
        candidates = []
        for item in sample["outbound"]:
            stream = _stream(item)
            if (all(stream[name] for name in ("width_available", "height_available", "fps_available"))
                    and stream["fps"] > 0
                    and (stream["width"], stream["height"]) == dimensions):
                candidates.append(stream)
        if len(candidates) != 1:
            return None
        candidate = candidates[0]
        name = mapped.get(candidate["rid"])
        if name is None:
            return None
        layer = source["layers"][name]
        return name if candidate["width"] <= layer["width"] and candidate["height"] <= layer["height"] else None
    except (KeyError, TypeError, ValueError):
        return None
