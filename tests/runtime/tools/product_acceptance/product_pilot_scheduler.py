"""Frozen task-child scheduling and reversible diagnostic-only accounting."""
from pathlib import Path
import hashlib
import json
import os
import re


SCHEDULER_POLICY = dict(schema=1, status='FROZEN_B14_TEST', diagnostic_only=False,
    scope='collector child and publisher descendants only',
    scheduler=dict(nice=0, policy=0),
    removed_capabilities=['CAP_SYS_NICE', 'CAP_SYS_RESOURCE'],
    no_new_privs=True, rtprio_limit=[0, 0])


def validate_scheduler_policy(value):
    if json.dumps(value, sort_keys=True) != json.dumps(SCHEDULER_POLICY, sort_keys=True):
        raise ValueError('task_scheduler_policy_not_frozen')
    return value


def load_scheduler_policy(path, diagnostic=False, nice=0):
    if path is None:
        return None
    if diagnostic or type(nice) is not int or nice != 0:
        raise ValueError('frozen_scheduler_requires_ordinary_nice_zero')
    raw = Path(path).read_bytes()
    return dict(sha256=hashlib.sha256(raw).hexdigest(),
                policy=validate_scheduler_policy(json.loads(raw.decode('utf-8-sig'))))


def validate_scheduler_metadata(policy):
    validate_scheduler_policy(policy['policy'])
    if (set(policy) != {'sha256', 'policy'} or not isinstance(policy['sha256'], str)
            or re.fullmatch(r'[0-9a-f]{64}', policy['sha256']) is None):
        raise ValueError('task_scheduler_policy_identity_invalid')
    return policy


def scheduler_policy_readback(policy):
    validate_scheduler_metadata(policy)
    scheduler = process_scheduler()
    if json.dumps(scheduler, sort_keys=True) != json.dumps(policy['policy']['scheduler'], sort_keys=True):
        raise ValueError('task_scheduler_policy_not_applied')
    return dict(policy_sha256=policy['sha256'], stage='pre_sdk_import',
                scheduler=scheduler, restriction=realtime_restriction_state())


def validate_scheduler_readback(value, policy):
    validate_scheduler_metadata(policy)
    expected = dict(policy_sha256=policy['sha256'], stage='pre_sdk_import',
                    scheduler=policy['policy']['scheduler'], restriction=expected_realtime_restriction())
    if json.dumps(value, sort_keys=True) != json.dumps(expected, sort_keys=True):
        raise ValueError('task_scheduler_readback_mismatch')
    return value


def review_scheduler_policy(plan, remote, route):
    """Recorded startup evidence; does not infer unmeasured thread policy samples."""
    try:
        policy = plan['collector_scheduler_policy']
        validate_scheduler_metadata(policy)
        if json.dumps(route.get('scheduler_policy'), sort_keys=True) != json.dumps(policy, sort_keys=True):
            raise ValueError('task_scheduler_route_identity_mismatch')
        receivers, publishers = [], []
        for row in remote:
            if row['event'] == 'receiver.scheduler_policy': receivers.append(row)
            if row['event'] == 'load.ready': publishers.append(row)
        if len(receivers) != 1 or len(publishers) != 1:
            raise ValueError('task_scheduler_readback_count')
        for row in receivers + publishers:
            if row['run_id'] != plan['run_id']:
                raise ValueError('task_scheduler_run_identity_mismatch')
        receiver = validate_scheduler_readback(receivers[0]['proof'], policy)
        publisher = validate_scheduler_readback(publishers[0]['publisher_process']['publisher_scheduler_policy'], policy)
        return dict(passed=True, policy_sha256=policy['sha256'], receiver=receiver, publisher=publisher)
    except (KeyError, TypeError, ValueError) as error:
        return dict(passed=False, reason=str(error))


def validate_receiver_priority(nice, diagnostic):
    if type(nice) is not int or not -10 <= nice <= 0:
        raise ValueError("diagnostic_receiver_nice_invalid")
    if nice != 0 and not diagnostic:
        raise ValueError("receiver_priority_requires_timing_diagnostic")
    return nice


def configure_receiver_priority(nice, diagnostic):
    validate_receiver_priority(nice, diagnostic)
    if not diagnostic:
        return
    if os.sched_getscheduler(0) != os.SCHED_OTHER:
        raise RuntimeError("receiver_requires_sched_other")
    os.setpriority(os.PRIO_PROCESS, 0, nice)
    if os.getpriority(os.PRIO_PROCESS, 0) != nice:
        raise RuntimeError("receiver_priority_not_applied")


def process_scheduler():
    return dict(nice=os.getpriority(os.PRIO_PROCESS, 0), policy=os.sched_getscheduler(0))


def validate_no_realtime(enabled, diagnostic, nice=0):
    if type(enabled) is not bool or (enabled and (not diagnostic or nice != 0)):
        raise ValueError('no_realtime_requires_timing_diagnostic_nice_zero')
    return enabled


def realtime_child_command(command):
    # Wrapper has already validated this exact task-owned Python child argv.
    return ['/usr/bin/setpriv','--bounding-set=-sys_nice,-sys_resource',
            '--inh-caps=-sys_nice,-sys_resource','--ambient-caps=-sys_nice,-sys_resource',
            '--no-new-privs','--',*command]


def restrict_realtime_limit():
    """Called only in the task wrapper's child, before exec and SDK threads."""
    import resource
    resource.setrlimit(resource.RLIMIT_RTPRIO,(0,0))


def realtime_restriction_state():
    import resource
    fields = dict(line.split(':',1) for line in Path('/proc/self/status').read_text().splitlines() if ':' in line)
    state = {name: bool(int(fields[field].strip(),16) & (1<<23))
        for name,field in (('cap_sys_nice_effective','CapEff'),('cap_sys_nice_permitted','CapPrm'),
                           ('cap_sys_nice_bounding','CapBnd'),('cap_sys_nice_inheritable','CapInh'),('cap_sys_nice_ambient','CapAmb'))}
    # Without this removal, a privileged SDK could raise the hard RTPRIO limit.
    state.update({name:bool(int(fields[field].strip(),16)&(1<<24))
        for name,field in (('cap_sys_resource_effective','CapEff'),('cap_sys_resource_permitted','CapPrm'),
            ('cap_sys_resource_bounding','CapBnd'),('cap_sys_resource_inheritable','CapInh'),('cap_sys_resource_ambient','CapAmb'))})
    state['no_new_privs'] = int(fields['NoNewPrivs'].strip()) == 1
    state['rtprio_limit'] = list(resource.getrlimit(resource.RLIMIT_RTPRIO))
    validate_realtime_restriction(state)
    return state


def expected_realtime_restriction():
    expected = dict(cap_sys_nice_effective=False,cap_sys_nice_permitted=False,
        cap_sys_nice_bounding=False,cap_sys_nice_inheritable=False,cap_sys_nice_ambient=False,
        no_new_privs=True,rtprio_limit=[0,0])
    expected.update({f'cap_sys_resource_{field}':False for field in ('effective','permitted','bounding','inheritable','ambient')})
    return expected


def validate_realtime_restriction(state):
    # Reject bool-as-integer and missing fields in independent readiness review.
    if json.dumps(state,sort_keys=True) != json.dumps(expected_realtime_restriction(),sort_keys=True):
        raise ValueError('task_realtime_restriction_not_applied')
    return state


class SchedulingStatsLease:
    def __init__(self, proof, setting=Path("/proc/sys/kernel/sched_schedstats"),
                 lock_path=Path("/run/lock/cohavora-b14-schedstats.lock")):
        self.proof = proof["scheduler_statistics"] = dict(diagnostic_only=True, release_eligible=False)
        self.setting, self.lock_path = setting, lock_path
        self.original = None
        self.changed = False
        self.lock = None
        self.locked = False

    def __enter__(self):
        import fcntl
        self.fcntl = fcntl
        try:
            self.lock = self.lock_path.open("a")
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.locked = True
            value = self.setting.read_text().strip()
            if value not in ("0", "1"):
                raise ValueError("invalid_schedstats_setting")
            self.original = value
            self.proof["original"] = value
            if value == "0":
                self.changed = True
                self.setting.write_text("1\n")
            self.proof["enabled"] = self.setting.read_text().strip()
            if self.proof["enabled"] != "1":
                raise RuntimeError("schedstats_enable_not_verified")
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.original is not None:
                current = self.setting.read_text().strip()
                if self.changed:
                    if current not in ("1", self.original):
                        raise RuntimeError("schedstats_setting_changed_unexpectedly")
                    if current != self.original:
                        self.setting.write_text(self.original + "\n")
                self.proof["restored"] = self.setting.read_text().strip()
                self.proof["cleanup_complete"] = self.proof["restored"] == self.original
                if not self.proof["cleanup_complete"]:
                    raise RuntimeError("schedstats_restore_not_verified")
        except BaseException as error:
            self.proof.update(cleanup_complete=False, cleanup_error_type=type(error).__name__)
            raise
        finally:
            if self.lock is not None:
                if self.locked:
                    self.fcntl.flock(self.lock, self.fcntl.LOCK_UN)
                self.lock.close()
                self.lock = None
                self.locked = False

    def __exit__(self, kind, value, traceback):
        self.close()
