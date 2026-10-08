"""API-only ownership checks for one diagnostic room; never imports RTC."""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import re
import sys
import time


class GateError(Exception):
    pass


def require(value, code):
    if not value:
        raise GateError(code)


def identities(run_id):
    require(re.fullmatch(r'[a-f0-9]{32}', run_id) is not None, 'INVALID_RUN_ID')
    return {f'pilot-{run_id[:8]}-load-{index:02d}' for index in range(10)}


def check_members(participants, run_id, subscriber=False, empty=False):
    expected = identities(run_id)
    if subscriber:
        expected.add('native-exit-' + run_id[:8] + '-subscriber')
    if empty:
        expected = set()
    actual = [participant.identity for participant in participants]
    require(len(actual) == len(expected) and set(actual) == expected,
            'PARTICIPANT_SET_MISMATCH')
    if not empty:
        videos = audios = 0
        for participant in participants:
            if participant.identity.endswith('-subscriber'):
                require(not participant.tracks, 'SUBSCRIBER_PUBLISHED_TRACK')
                continue
            camera = microphone = 0
            for track in participant.tracks:
                require(not track.muted, 'PUBLISHER_TRACK_MUTED')
                field = track.DESCRIPTOR.fields_by_name['source']
                source = field.enum_type.values_by_number[int(track.source)].name
                if source == 'CAMERA' and track.name == 'pilot-low-vp8':
                    require((track.width, track.height) == (160, 90), 'PUBLISH_GEOMETRY_CHANGED')
                    camera += 1
                elif source == 'MICROPHONE' and track.name == 'pilot-tone':
                    microphone += 1
                else:
                    raise GateError('UNEXPECTED_TRACK')
            require((camera, microphone) == (1, int(participant.identity.endswith('-00'))),
                    'PUBLISHER_TRACK_COUNTS_CHANGED')
            videos += camera
            audios += microphone
        require((videos, audios) == (10, 1), 'TOTAL_TRACK_COUNTS_CHANGED')
    return len(actual)


def validate_bundle(plan, root, bundle, run_id):
    require(bundle in {root / ('native-exit-' + run_id) / leaf for leaf in ('exit-capture', 'full-exit-capture')}, 'BUNDLE_NOT_OWNED')
    if bundle.name == 'full-exit-capture':
        require(plan.get('output') == str(bundle) and plan.get('diagnostic_profile') in
                ('full-exit-660-two-240', 'full-exit-all-660-two-240'), 'FULL_BUNDLE_CONTEXT_MISMATCH')


async def execute(plan, mode):
    root = Path(plan['remote_root'])
    bundle = Path(plan['bundle_root'])
    run_id = plan['run_id']
    identities(run_id)
    require(plan['room_name'] == 'native-exit-' + run_id, 'ROOM_NAME_NOT_OWNED')
    validate_bundle(plan, root, bundle, run_id)
    sys.path.insert(0, str(root / 'collector-python'))
    from livekit import api
    import yaml
    config = yaml.safe_load(Path(plan['target']['livekit_config']).read_text())
    key, secret = next(iter(config['keys'].items()))
    client = api.LiveKitAPI(url=plan['api_url'], api_key=key, api_secret=secret)
    owner_file = bundle / 'api-room-owner.json'

    async def call(awaitable):
        return await asyncio.wait_for(awaitable, 5)

    async def room_list():
        response = await call(client.room.list_rooms(api.ListRoomsRequest(names=[plan['room_name']])))
        require(len(response.rooms) <= 1, 'ROOM_AMBIGUOUS')
        require(all(room.name == plan['room_name'] for room in response.rooms), 'ROOM_NAME_MISMATCH')
        return list(response.rooms)

    async def owned_room():
        owner = json.loads(owner_file.read_text())
        require(owner['run_id'] == run_id, 'OWNER_RUN_ID_MISMATCH')
        rooms = await room_list()
        if not rooms:
            return None
        require(hashlib.sha256(rooms[0].sid.encode()).hexdigest() == owner['sid_sha256'], 'ROOM_SID_CHANGED')
        return rooms[0]

    async def participants():
        response = await call(client.room.list_participants(api.ListParticipantsRequest(room=plan['room_name'])))
        return list(response.participants)

    try:
        if mode == 'check-plan':
            require(not owner_file.exists(), 'OWNER_RECEIPT_ALREADY_EXISTS')
            require(not await room_list(), 'ROOM_ALREADY_EXISTS')
            room = await call(client.room.create_room(api.CreateRoomRequest(name=plan['room_name'],
                empty_timeout=259200, departure_timeout=259200)))
            require(room.name == plan['room_name'], 'CREATED_ROOM_NAME_MISMATCH')
            # Record ownership before further observations, so cleanup can recover
            # a creation that succeeded but a later observation failed.
            with owner_file.open('x', encoding='utf-8') as stream:
                json.dump(dict(schema=1, run_id=run_id,
                    sid_sha256=hashlib.sha256(room.sid.encode()).hexdigest()), stream)
            check_members(await participants(), run_id, empty=True)
            return dict(status='COMPLETE', mode=mode, participants=0, created_owned_room=True)
        room = await owned_room()
        if mode == 'cleanup':
            if room is None:
                return dict(status='COMPLETE', mode=mode, room_absent=True)
            deadline = time.monotonic() + 25
            allowed = identities(run_id) | {'native-exit-' + run_id[:8] + '-subscriber'}
            while True:
                rows = await participants()
                require(all(row.identity in allowed for row in rows), 'FOREIGN_PARTICIPANT_PRESENT')
                if not rows:
                    break
                require(time.monotonic() < deadline, 'OWNED_ROOM_NOT_EMPTY')
                await asyncio.sleep(.2)
                require(await owned_room() is not None, 'ROOM_DISAPPEARED_DURING_CLEANUP')
            require(await owned_room() is not None, 'ROOM_DISAPPEARED_BEFORE_DELETE')
            await call(client.room.delete_room(api.DeleteRoomRequest(room=plan['room_name'])))
            require(not await room_list(), 'ROOM_DELETE_NOT_OBSERVED')
            return dict(status='COMPLETE', mode=mode, room_absent=True, deleted_owned_empty_room=True)
        require(room is not None, 'OWNED_ROOM_MISSING')
        count = check_members(await participants(), run_id, subscriber=(mode == 'subscriber-active'))
        return dict(status='COMPLETE', mode=mode, participants=count, same_room_sid=True)
    finally:
        await asyncio.wait_for(client.aclose(), 5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    options = parser.add_mutually_exclusive_group(required=True)
    for mode in ('check-plan', 'publisher-ready', 'subscriber-active', 'post-subscriber', 'cleanup'):
        options.add_argument('--' + mode, dest='mode', action='store_const', const=mode)
    args = parser.parse_args()
    try:
        plan = json.loads(args.plan.read_text())
        result = asyncio.run(execute(plan, args.mode))
    except Exception as exc:
        result = dict(status='FAILED', mode=args.mode,
            code=str(exc) if isinstance(exc, GateError) else 'API_OR_INPUT_FAILED',
            error_type=type(exc).__name__)
    print(json.dumps(result))
    return 0 if result['status'] == 'COMPLETE' else 1


if __name__ == '__main__':
    raise SystemExit(main())
