#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import os
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='gcrp-qt-catalog-') as temporary:
    root = Path(temporary)
    test2 = root / 'test2'
    (test2 / 'device/case').mkdir(parents=True)
    (test2 / 'device/case/run.tcl').write_text('# fixture')
    (test2 / 'vivado_runner/runtime').mkdir(parents=True)
    (test2 / 'vivado_runner/runtime/run.tcl').write_text('# not a testcase')
    (test2 / '.svn').mkdir()
    (test2 / '.svn/run.tcl').write_text('# ignored')
    (root / 'outside').mkdir()
    (root / 'outside/run.tcl').write_text('# outside')
    (test2 / 'linked').symlink_to(root / 'outside', target_is_directory=True)
    templates = root / 'templates'
    templates.mkdir()
    (templates / 'route.json').write_text(json.dumps({'flow_config': {'route_design': 1}}))
    archives = root / 'zip'
    archives.mkdir()
    (archives / 'GalaxCore_12.zip').write_bytes(b'fixture')
    (archives / 'GalaxCore_2.zip').write_bytes(b'fixture')
    output = root / 'catalog.json'
    config = root / 'config.conf'
    config.write_text('server.work_root={}\nserver.templates_dir={}\nserver.artifact_root={}\ngui.catalog_file={}\n'.format(test2, templates, archives, output))
    environment = dict(os.environ, GCRP_CONFIG=str(config))
    subprocess.check_call([sys.executable, '-B', sys.argv[1]], env=environment)
    data = json.loads(output.read_text())
    if data['cases'] != ['device/case/run.tcl'] or data['revisions'] != ['12', '2']:
        raise RuntimeError('catalog scope or numeric revision ordering failed')
    if data['templates']['route']['flow_config'] != {'route_design': 1}:
        raise RuntimeError('flow snapshot changed')
print('QT_CATALOG_TEST PASS')
