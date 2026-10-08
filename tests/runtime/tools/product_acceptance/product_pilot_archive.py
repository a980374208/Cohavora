"""Lossless checkpoint storage; native bytes and revision proofs stay unchanged."""
import gzip
import hashlib
import io
import re
import zlib


MAXIMUM_SEGMENT_BYTES = 64 * 1024 * 1024


def encode_segment(name, content):
    if not re.fullmatch(r"segment-[0-9]{20}\.jsonl", name):
        raise ValueError("invalid_archive_segment_name")
    if not 0 < len(content) <= MAXIMUM_SEGMENT_BYTES:
        raise ValueError("archive_segment_size_invalid")
    stored = gzip.compress(content, compresslevel=1, mtime=0)
    return stored, dict(archive_file=name + ".gz", archive_encoding="gzip",
                       stored_bytes=len(stored), stored_sha256=hashlib.sha256(stored).hexdigest())


def read_segment(directory, entry):
    name = entry["file"]
    size = entry["size_bytes"]
    if not re.fullmatch(r"segment-[0-9]{20}\.jsonl", name):
        raise ValueError("invalid_archive_segment_name")
    if type(size) is not int or not 0 < size <= MAXIMUM_SEGMENT_BYTES:
        raise ValueError("archive_segment_size_invalid")
    encoding = entry.get("archive_encoding", "raw")
    if encoding == "gzip":
        if entry.get("archive_file") != name + ".gz":
            raise ValueError("invalid_archive_storage_path")
        filename = entry["archive_file"]
    elif encoding == "raw" and "archive_file" not in entry:
        filename = name
    else:
        raise ValueError("invalid_archive_encoding")
    path = directory / filename
    if path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
        raise ValueError("archive_path_escape")
    with path.open("rb") as stream:
        stored = stream.read(MAXIMUM_SEGMENT_BYTES + 1024 * 1024 + 1)
    if len(stored) > MAXIMUM_SEGMENT_BYTES + 1024 * 1024:
        raise ValueError("archive_storage_size_invalid")
    if encoding == "gzip":
        if len(stored) != entry.get("stored_bytes") or hashlib.sha256(stored).hexdigest() != entry.get("stored_sha256"):
            raise ValueError("archive_storage_hash_or_size_mismatch")
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(stored)) as stream:
                content = stream.read(size + 1)
        except (OSError, EOFError, zlib.error) as error:
            raise ValueError("archive_compressed_payload_invalid") from error
    else:
        content = stored
    if len(content) != size or hashlib.sha256(content).hexdigest() != entry["sha256"]:
        raise ValueError("archive_hash_or_size_mismatch")
    return content
