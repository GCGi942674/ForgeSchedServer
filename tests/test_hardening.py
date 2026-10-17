"""Durable log intent and conservative service-log retention; no real tasks."""
import json
import os
from pathlib import Path
import sys
import tempfile
import sqlite3
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'worker'))
sys.path.insert(0, str(ROOT / 'tools'))
from gcrp_worker import Worker, TransportError
from service_log import prune
from backup_database import backup_database
from health_check import storage_health


class HardeningTests(unittest.TestCase):
    def test_capacity_warning_without_deleting_data(self):
        with tempfile.TemporaryDirectory() as folder:
            config={'worker.output_root':folder,'worker.slots_root':folder,'health.min_free_bytes':'1000'}
            with mock.patch('health_check.shutil.disk_usage',return_value=SimpleNamespace(free=10,total=10000)):
                result=storage_health(config,'worker')
                self.assertEqual(result['verdict'],'WARNING')
                self.assertEqual(result['storage']['slots']['status'],'LOW_SPACE')
    def test_sqlite_wal_backup_and_python36_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'state.db'
            with sqlite3.connect(str(source)) as writer:
                writer.execute('PRAGMA journal_mode=WAL')
                writer.execute('CREATE TABLE tasks(id INTEGER PRIMARY KEY, result TEXT)')
                writer.execute("INSERT INTO tasks VALUES(1,'PASS')")
                writer.execute('PRAGMA user_version=3')
                writer.commit()
                for logical in (False, True):
                    root = Path(folder) / str(logical)
                    path = backup_database({'database.path': str(source), 'database.backup_root': str(root), 'database.min_free_bytes':'0'}, logical=logical)
                    with sqlite3.connect(str(path)) as restored:
                        self.assertEqual(restored.execute('SELECT result FROM tasks').fetchone()[0], 'PASS')
                        self.assertEqual(restored.execute('PRAGMA user_version').fetchone()[0], 3)
    def test_result_keeps_archive_retry_intent(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            directory = output / 'run-test' / '1'
            directory.mkdir(parents=True)
            (directory / 'stdout.log').write_text('evidence')
            worker = Worker(SimpleNamespace(output=output, worker_id='host-slot1',
                pjtest_config=None, pjtest_paths={}))
            assignment = dict(task_id=1, run_id='run-1')
            worker.save_report(dict(task_id=1, run_id='run-1', worker_id='host-slot1',
                status='SUCCEEDED', message='{}',
                _archive=dict(assignment=assignment, directory=str(directory))))
            with mock.patch.object(worker, 'queue_logs', side_effect=OSError('disk full')), mock.patch.object(worker, 'request') as request:
                with self.assertRaises(TransportError):
                    worker.flush_report()
                request.assert_not_called()
            self.assertTrue(worker.pending_path().exists())
            with mock.patch.object(worker, 'request', return_value={}) as request:
                worker.flush_report()
                self.assertNotIn('_archive', request.call_args[0][1])
            self.assertFalse(worker.pending_path().exists())
            self.assertTrue((output / 'pending-logs-run-1.json').is_file())
            worker.queue_logs(assignment, directory)  # Same intent is idempotent.

    def test_service_retention_preserves_live_and_unknown_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            day = root / '20000101'
            day.mkdir()
            expired = day / 'worker-service.1.log'
            live = day / 'worker-service.2.log'
            unknown = day / 'important.log'
            for path in (expired, live, unknown):
                path.write_text('log')
                os.utime(str(path), (1, 1))
            with live.open('rb'):
                prune(root, 'worker', 30)
                self.assertTrue(live.exists())
            self.assertFalse(expired.exists())
            self.assertTrue(unknown.exists())


if __name__ == '__main__':
    unittest.main()
