#!/usr/bin/env python3
"""Build and check the C++ Qt5 GUI; no Server/Workers required (Python 3.6)."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'worker'))
from runtime_config import load_config


def tool(name):
    intranet = Path('/home/xshare/scripts/bin/cmake-3.15.2/bin') / name
    return str(intranet) if intranet.is_file() else shutil.which(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--show-demo', action='store_true', help='also open a visible demo window')
    parser.add_argument('--connect', action='store_true', help='also probe the configured Server using read-only queries')
    args = parser.parse_args()
    report_dir = ROOT / 'intranet-report' / 'qt-gui'
    report_dir.mkdir(parents=True, exist_ok=True)
    report = {'verdict': 'INCOMPLETE', 'started_at': datetime.now(timezone.utc).isoformat()}
    try:
        config = load_config()
        cmake, ctest = tool('cmake'), tool('ctest')
        if not cmake or not ctest:
            raise ValueError('CMake and ctest are required')
        build = ROOT / 'build' / 'gui'
        def run(command, cwd, name, env=None, timeout=300):
            log = report_dir / (name + '.log')
            with log.open('w', encoding='utf-8') as stream:
                code = subprocess.run(command, cwd=str(cwd), env=env,
                                      stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=timeout, check=False).returncode
            report[name] = {'exit_code': code, 'log': str(log)}
            if code:
                raise ValueError(name + ' failed; see ' + str(log))
        print('[QT-GUI] configuring Qt5 (5.8+) Widgets + Network', flush=True)
        command = [cmake, '-S', str(ROOT / 'gui' / 'qt'), '-B', str(build), '-DBUILD_TESTING=ON']
        if config.get('gui.qt_prefix'):
            command.append('-DCMAKE_PREFIX_PATH=' + config['gui.qt_prefix'])
        run(command, ROOT, 'configure')
        print('[QT-GUI] building', flush=True)
        run([cmake, '--build', str(build), '--parallel', '8'], ROOT, 'build', timeout=600)
        print('[QT-GUI] transport + offscreen widget tests', flush=True)
        run([ctest, '--output-on-failure'], build, 'tests')
        if args.connect:
            print('[QT-GUI] read-only Server probe', flush=True)
            probe_env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
            run([str(build / 'bin' / 'forgesched_gui'), '--probe'], ROOT, 'server_probe', env=probe_env)
        report['display'] = os.environ.get('DISPLAY', '')
        report['wayland_display'] = os.environ.get('WAYLAND_DISPLAY', '')
        qmake = config.get('tools.qt_qmake') or shutil.which('qmake')
        if qmake:
            result = subprocess.run([qmake, '-query', 'QT_VERSION'], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=10, check=False)
            report['qmake_version'] = result.stdout.decode('utf-8', 'replace').strip()
            report['qmake'] = qmake
        report['verdict'] = 'OFFSCREEN_PASS'
        if args.show_demo:
            if not report['display'] and not report['wayland_display']:
                raise ValueError('no DISPLAY/WAYLAND_DISPLAY; open a desktop terminal first')
            print('[QT-GUI] close the demo window to finish', flush=True)
            run([str(build / 'bin' / 'forgesched_gui'), '--demo'], ROOT, 'visible_demo', timeout=3600)
            report['verdict'] = 'PASS'
        else:
            report['note'] = 'Headless tests passed; visible desktop/plugin compatibility still needs --show-demo.'
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report['verdict'] = 'FAIL'
        report['error'] = str(error)
    report['finished_at'] = datetime.now(timezone.utc).isoformat()
    path = report_dir / 'report.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('[QT-GUI] {} report={}'.format(report['verdict'], path), flush=True)
    return 1 if report['verdict'] == 'FAIL' else 0


if __name__ == '__main__':
    sys.exit(main())
