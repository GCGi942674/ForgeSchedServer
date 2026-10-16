#!/usr/bin/env python3
"""Capture all Qt UI v3 review screens offscreen. No Server/Worker is contacted."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCREENS = ('tasks', 'workers', 'add', 'regression', 'batch-detail',
           'flow-config', 'original-run', 'export')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default=str(ROOT / 'build/bin/gcrp_gui'))
    parser.add_argument('--output-dir', default=str(ROOT / 'intranet-report/qt-v3'))
    args = parser.parse_args()
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {'verdict': 'INCOMPLETE', 'started_at': datetime.now(timezone.utc).isoformat(),
              'mode': 'isolated_demo_offscreen', 'screens': {},
              'desktop_animation_validation': 'NOT_RUN'}
    try:
        binary = Path(args.binary).resolve()
        if not binary.is_file():
            raise ValueError('GUI binary missing; build gcrp_gui first')
        digest = hashlib.sha256()
        with binary.open('rb') as stream:
            for block in iter(lambda: stream.read(65536), b''):
                digest.update(block)
        report['binary'] = str(binary)
        report['binary_sha256'] = digest.hexdigest()
        screenshots = output / 'screenshots'
        screenshots.mkdir(exist_ok=True)
        with (output / 'render.log').open('w', encoding='utf-8') as log:
            result = subprocess.run([str(binary), '--demo', '--review-dir', str(screenshots)],
                cwd=str(ROOT), env=dict(os.environ, QT_QPA_PLATFORM='offscreen'),
                stdout=log, stderr=subprocess.STDOUT, timeout=90, check=False)
        if result.returncode:
            raise ValueError('Qt screenshot capture failed; inspect render.log')
        for name in SCREENS:
            path = screenshots / (name + '.png')
            with path.open('rb') as stream:
                header = stream.read(24)
            if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n':
                raise ValueError('invalid PNG: ' + name)
            width, height = struct.unpack('!II', header[16:24])
            if width < 600 or height < 400 or path.stat().st_size < 1000:
                raise ValueError('unexpected screenshot size: ' + name)
            report['screens'][name] = {'path': str(path), 'width': width, 'height': height}
        report['verdict'] = 'OFFSCREEN_PASS'
        report['note'] = 'Eight screens rendered. Visual fidelity must be reviewed from PNGs; '
        report['note'] += 'this does not certify desktop fonts, plugins or animation smoothness.'
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report['verdict'] = 'FAIL'
        report['error'] = str(error)
    report['finished_at'] = datetime.now(timezone.utc).isoformat()
    path = output / 'report.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('[QT-UI-V3] {} report={}'.format(report['verdict'], path), flush=True)
    return 0 if report['verdict'] == 'OFFSCREEN_PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
