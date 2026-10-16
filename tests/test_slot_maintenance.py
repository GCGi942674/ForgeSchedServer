"""Maintenance journal invariants; no proprietary tool or remote service."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'worker'))
from maintenance import SlotMaintenance
from recovery_journal import write_json


class MaintenanceTests(unittest.TestCase):
    def test_replay_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calls = []
            class Adapter:
                def update_checkout(self, scope, executor, log):
                    calls.append(scope)
            command = dict(operation_id='replay-key', scope='test2')
            maintenance = SlotMaintenance(root, lambda: None)
            maintenance.offer(command)
            self.assertTrue(maintenance.run_if_idle(Adapter()))
            self.assertEqual(maintenance.acknowledgement()['state'], 'SUCCEEDED')
            restored = SlotMaintenance(root, lambda: None)
            restored.offer(command)
            self.assertFalse(restored.run_if_idle(Adapter()))
            self.assertEqual(calls, ['test2'])
            restored.offer(dict(operation_id='unsupported-demo', scope='all'))
            self.assertTrue(restored.run_if_idle(None))
            self.assertEqual(restored.acknowledgement()['state'], 'FAILED')
            for bad in (dict(command, scope='../other'), dict(command, operation_id='../escape'), dict(command, command='rm')):
                with self.assertRaises(ValueError):
                    restored.offer(bad)

    def test_exit_proof_and_quarantine(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state = dict(operation_id='interrupted', scope='all', state='RUNNING', error='', receipt=None)
            write_json(root / 'maintenance.json', state)
            with self.assertRaises(RuntimeError):
                SlotMaintenance(root, lambda: None)
            receipt = root / 'maintenance-receipt-fixture.json'
            write_json(receipt, dict(clean=True, token=receipt.name))
            state['receipt'] = str(receipt)
            write_json(root / 'maintenance.json', state)
            restored = SlotMaintenance(root, lambda: None)
            self.assertEqual(restored.acknowledgement()['state'], 'FAILED')
            self.assertIn('interrupted', restored.acknowledgement()['error'])

    def test_unknown_process_exit_is_never_acknowledged_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            class Unsafe:
                def update_checkout(self, scope, executor, log):
                    executor.safe = False
                    raise RuntimeError('supervisor lost')
            maintenance = SlotMaintenance(directory, lambda: None)
            maintenance.offer(dict(operation_id='unsafe', scope='test2'))
            with self.assertRaises(RuntimeError):
                maintenance.run_if_idle(Unsafe())
            self.assertEqual(json.loads((Path(directory) / 'maintenance.json').read_text())['state'], 'RUNNING')

    def test_utf8_error_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            class Broken:
                def update_checkout(self, scope, executor, log):
                    raise RuntimeError('中文错误' * 500)
            maintenance = SlotMaintenance(directory, lambda: None)
            maintenance.offer(dict(operation_id='utf8', scope='test2'))
            maintenance.run_if_idle(Broken())
            self.assertLessEqual(len(maintenance.acknowledgement()['error'].encode('utf-8')), 512)
            SlotMaintenance(directory, lambda: None)


if __name__ == '__main__':
    unittest.main()
