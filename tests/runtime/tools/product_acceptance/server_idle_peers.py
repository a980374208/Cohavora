"""Bounded non-publishing SFU peers for the room-count codec policy fixture."""
from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import re
import time

from livekit import rtc


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--room", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=1200)
    args = parser.parse_args()
    if not re.fullmatch(r"b03-[0-9a-f]{32}", args.room) or not 30 <= args.seconds <= 1200:
        raise ValueError("idle_fixture_scope_invalid")
    tokens = json.loads(os.environ.pop("COHAVORA_IDLE_TOKENS"))
    if len(tokens) != 3:
        raise ValueError("idle_fixture_count_invalid")
    args.output.mkdir(mode=0o700, parents=True, exist_ok=True)
    rooms = [rtc.Room() for _ in tokens]
    state = dict(room=args.room, count=0, status="STARTING", pid=os.getpid())
    path = args.output / "state.json"

    def save():
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(state), encoding="utf-8")
        temporary.replace(path)

    save()
    try:
        for room, token in zip(rooms, tokens):
            await room.connect(os.environ["COHAVORA_IDLE_URL"], token,
                               options=rtc.RoomOptions(auto_subscribe=False))
            state["count"] += 1
        state.update(status="READY", media_publications=sum(
            len(room.local_participant.track_publications) for room in rooms))
        save()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline and not (args.output / "stop").exists():
            await asyncio.sleep(.25)
    except Exception as error:
        state.update(status="FAIL", error_type=type(error).__name__)
        save()
    finally:
        await asyncio.gather(*(room.disconnect() for room in rooms), return_exceptions=True)
        state["terminal"] = "CLOSED"
        save()


if __name__ == "__main__":
    asyncio.run(main())
