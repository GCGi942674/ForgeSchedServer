"""Role config selection, non-destructive migration, and Clock logs; no real workers."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'worker'), str(ROOT / 'server'), str(ROOT / 'tools')]
from runtime_config import load_config
from split_config import split
from daily_log import daily_log, prune


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.cwd = Path.cwd()
        self.override = os.environ.pop('GCRP_CONFIG', None)
        self.config = self.base / 'config'
        self.config.mkdir()
        os.chdir(str(self.base))

    def tearDown(self):
        os.chdir(str(self.cwd))
        os.environ.pop('GCRP_CONFIG', None)
        if self.override is not None:
            os.environ['GCRP_CONFIG'] = self.override
        self.temp.cleanup()

    def test_roles_legacy_override_and_subdirectory(self):
        (self.config / 'gcrp.conf').write_text('server.port=1\n', encoding='utf-8')
        self.assertEqual(load_config('worker')['server.port'], '1')
        (self.config / 'server.conf').write_text('server.port=2\n', encoding='utf-8')
        (self.config / 'worker.conf').write_text('server.port=3\n', encoding='utf-8')
        (self.base / 'build').mkdir()
        os.chdir(str(self.base / 'build'))
        self.assertEqual(load_config()['server.port'], '2')
        self.assertEqual(load_config('worker')['server.port'], '3')
        os.environ['GCRP_CONFIG'] = str(self.config / 'gcrp.conf')
        self.assertEqual(load_config('worker')['server.port'], '1')
        os.environ['GCRP_CONFIG'] = ''
        with self.assertRaises(ValueError):
            load_config()
        os.environ['GCRP_CONFIG'] = str(self.base / 'absent.conf')
        with self.assertRaises(OSError):
            load_config()
        with self.assertRaises(ValueError):
            load_config('unknown')

    def test_split_preserves_paths_and_never_overwrites(self):
        legacy = self.config / 'gcrp.conf'
        text = 'database.path=./data/existing.db\nworker.output_root=./existing-output\nserver.port=8080\nsmoke.revision=19037\nworker.svn_url=http://example/svn\n'
        legacy.write_text(text, encoding='utf-8')
        split(legacy, self.config)
        self.assertEqual(load_config()['database.path'], './data/existing.db')
        self.assertNotIn('worker.svn_url', load_config())
        self.assertEqual(load_config('worker')['worker.output_root'], './existing-output')
        self.assertEqual(load_config('worker')['server.port'], '8080')
        self.assertEqual(legacy.read_text(encoding='utf-8'), text)
        before = (self.config / 'server.conf').read_bytes()
        with self.assertRaises(ValueError):
            split(legacy, self.config)
        self.assertEqual((self.config / 'server.conf').read_bytes(), before)

    def test_daily_capture_retention_and_failure(self):
        root = self.base / 'logs/daily'
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            with daily_log({'server.daily_log_root': str(root)}) as path:
                print('SUBMITTED: mock batch')
                print('mock error', file=sys.stderr)
        text = path.read_text(encoding='utf-8')
        self.assertIn('SUBMITTED', text)
        self.assertIn('mock error', text)
        old = root / '20000101'
        old.mkdir()
        expired = old / 'clock-010203-123-abcdef01.log'
        expired.write_text('expired', encoding='utf-8')
        keep = old / 'user-notes.txt'
        keep.write_text('keep', encoding='utf-8')
        os.utime(str(expired), (1, 1))
        prune(root, 30)
        self.assertFalse(expired.exists())
        self.assertTrue(keep.exists())
        self.assertTrue(path.exists())
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                with daily_log({'server.daily_log_root': str(root)}) as failed:
                    print('before failure')
                    raise RuntimeError('fixture')
        self.assertIn('invocation finished', failed.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
