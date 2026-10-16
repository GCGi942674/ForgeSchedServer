#!/usr/bin/env python3
"""Isolated Linux launcher test. Never starts a real Server or Worker."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux /proc and flock required')
class LauncherTest(unittest.TestCase):
    def test_worker_lifecycle(self):
        source = Path(__file__).resolve().parents[1] / 'tools/service_control.py'
        with tempfile.TemporaryDirectory(prefix='gcrp-launch-test-') as folder:
            root = Path(folder)
            for directory in ('tools', 'worker', 'config'):
                (root / directory).mkdir()
            shutil.copyfile(str(source), str(root / 'tools/service_control.py'))
            (root / 'config/worker.conf').write_text('worker.slots=1\n')
            (root / 'worker/slot_manager.py').write_text(
                'import signal,time\n'
                'running=True\n'
                'def stop(*args):\n global running\n running=False\n'
                'signal.signal(signal.SIGTERM,stop)\n'
                'signal.signal(signal.SIGHUP,lambda *a:None)\n'
                'while running: time.sleep(.05)\n')
            command = [sys.executable, str(root / 'tools/service_control.py'), 'worker']

            def run(*args):
                return subprocess.run(command + list(args), stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, timeout=10)

            try:
                started = run('start')
                self.assertEqual(started.returncode, 0, started.stdout)
                record = root / 'data/services/worker.json'
                first = json.loads(record.read_text())
                self.assertEqual(run('start').returncode, 0)
                self.assertEqual(first['pid'], json.loads(record.read_text())['pid'])
                self.assertEqual(run('status').returncode, 0)
                self.assertEqual(run('reload').returncode, 0)
                self.assertEqual(run('stop', '--wait', '5').returncode, 0)
                self.assertEqual(run('status').returncode, 3)
                # Reused/wrong PID must never be signalled.
                first['pid'] = __import__('os').getpid()
                record.write_text(json.dumps(first))
                self.assertEqual(run('stop').returncode, 0)
                self.assertEqual(run('status').returncode, 3)
            finally:
                run('stop', '--wait', '5')


if __name__ == '__main__':
    unittest.main()
