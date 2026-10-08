"""Offline fail-closed checks; never imports RTC or contacts a server."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

PATH=Path(__file__).parents[1]/'tools/diagnostics/native_exit/run_publisher_exit_capture.py'
spec=importlib.util.spec_from_file_location('exit_capture',PATH)
if sys.platform=='win32': sys.modules.setdefault('resource',types.ModuleType('resource'))
driver=importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
FROZEN_PUBLISHER=PATH.parent/'fixtures/product_pilot_load.py'
CURRENT_PUBLISHER=PATH.parents[2]/'product_acceptance/product_pilot_load.py'
COMPANIONS=('product_pilot_scheduler.py','product_pilot_timing.py',
            'product_pilot_video_counter.py')
SUBSCRIBER_COMPANIONS=('product_pilot_scheduler.py','product_pilot_video_counter.py')

def write_plan(directory,publisher_source=None):
    root=Path(directory)
    publisher=root/'product_pilot_load.py'
    publisher.write_bytes(FROZEN_PUBLISHER.read_bytes() if publisher_source is None else publisher_source)
    names=[*COMPANIONS,'wrapper.py','api.py','scheduler_policy.json',
           *[f'sdk-{i}.py' for i in range(5)],'liblivekit_ffi.so']
    for name in names:
        (root/name).write_bytes(('offline placeholder '+name).encode())
    subscriber_root=root/'subscriber-bundle'
    subscriber_root.mkdir()
    for name in ['subscriber.py',*SUBSCRIBER_COMPANIONS]:
        (subscriber_root/name).write_bytes(('offline subscriber placeholder '+name).encode())
    run_id='a'*32
    plan=dict(run_id=run_id,room='native-exit-'+run_id,
              output=str(Path('/root/livekit-product-acceptance')/('native-exit-'+run_id)/'exit-capture'),
              publisher_path=str(publisher),wrapper_path=str(root/'wrapper.py'),
              subscriber_path=str(subscriber_root/'subscriber.py'),api_helper_path=str(root/'api.py'),
              scheduler_policy=str(root/'scheduler_policy.json'),
              sdk_files=[str(root/name) for name in names[-6:]])
    plan['files']={str(root/name):driver.digest(root/name) for name in ['product_pilot_load.py',*names]}
    plan['files'].update({str(subscriber_root/name):driver.digest(subscriber_root/name)
                          for name in ['subscriber.py',*SUBSCRIBER_COMPANIONS]})
    path=root/'plan.json'
    path.write_text(json.dumps(plan),encoding='utf-8')
    return plan,path

class CaptureTest(unittest.TestCase):
    def test_complete_historical_manifest_and_missing_import_dependencies(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict(driver.os.environ,LD_PRELOAD='',LIVEKIT_LIB_PATH=''):
            plan,path=write_plan(directory)
            self.assertEqual(driver.load_plan(path,driver.digest(path)),plan)
            required=[*(Path(directory)/name for name in COMPANIONS),
                      *(Path(directory)/'subscriber-bundle'/name for name in SUBSCRIBER_COMPANIONS)]
            for dependency in required:
                changed=dict(plan,files=dict(plan['files']))
                del changed['files'][str(dependency)]
                path.write_text(json.dumps(changed),encoding='utf-8')
                with self.subTest(missing=str(dependency)),self.assertRaisesRegex(ValueError,'manifest_incomplete'):
                    driver.load_plan(path,driver.digest(path))

    def test_current_publisher_is_not_historical_reproduction_input(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict(driver.os.environ,LD_PRELOAD='',LIVEKIT_LIB_PATH=''):
            plan,path=write_plan(directory,CURRENT_PUBLISHER.read_bytes())
            self.assertNotEqual(plan['files'][plan['publisher_path']],driver.PUBLISHER_SHA)
            with self.assertRaisesRegex(ValueError,'publisher_changed'):
                driver.load_plan(path,driver.digest(path))

    def test_geometry_difference_is_not_failure(self):
        driver.validate_subscriber(dict(video_tracks=10,audio_tracks=1,
            minimum_video_frames=50,audio_frames=500),dict(schema=1,
            status='EXIT_DIAGNOSTIC_COMPLETE_GEOMETRY_DIFFERENCE_RECORDED',
            stop_requested=True,disconnected=True,all_streams_closed=True,errors=[]))

    def test_native_count_failure_is_retained(self):
        with self.assertRaisesRegex(ValueError,'native_media_measurement_failed'):
            driver.validate_subscriber(dict(video_tracks=10,audio_tracks=1,
                minimum_video_frames=49,audio_frames=500),{})

    def test_plan_requires_frozen_hash_before_parsing(self):
        with self.assertRaisesRegex(ValueError,'plan_hash_mismatch'):
            driver.load_plan(PATH,'0'*64)

if __name__=='__main__': unittest.main()
