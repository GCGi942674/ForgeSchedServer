"""Real TCP denial of unauthenticated administration; ordinary work still runs."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from test_sqlite_worker import request, wait_server, stop


def eventually(check):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if check():
            return
        time.sleep(.05)
    raise AssertionError('condition timed out')


with tempfile.TemporaryDirectory(prefix='gcrp-admin-denied-') as folder:
    root=Path(folder)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0))
        port=probe.getsockname()[1]
    config=root/'config.conf'
    config.write_text('server.bind_ip=127.0.0.1\nnetwork.server_ip=127.0.0.1\nserver.port={}\n'
        'server.io_threads=1\nserver.worker_threads=2\nlog.level=ERROR\nlog.dir={}\n'
        'database.path={}\nserver.log_archive_root={}\nworker.output_dir={}\n'
        'worker.reconnect_seconds=0.1\n'.format(port,root/'logs',root/'state.db',root/'archive',root/'output'))
    environment=dict(os.environ,GCRP_CONFIG=str(config))
    server=worker=None
    with (root/'console.log').open('w+') as log:
        def call(kind,data):
            result=request(port,kind,data)
            assert result['code']==0,result
            return result['result']
        def row():
            return next((w for w in call('query_workers',{})['items'] if w['worker_id']=='fixture-slot1'),None)
        def task(identity):
            return call('query_task',{'task_id':identity})
        def deny_all():
            for target in ({'machine_id':'fixture'},{'worker_id':'fixture-slot1'}):
                for action in ('pause','resume','update'):
                    data=dict(target,action=action,operation_id='denied-'+action)
                    if action=='update':
                        data['scope']='all'
                    result=request(port,'control_workers',data)
                    assert result['code']==3 and 'disabled until authentication' in result['message'],result
            assert row()['accepting_tasks'] is True
            assert row()['maintenance']['state']=='IDLE'
            assert not (root/'output/maintenance.json').exists()
        try:
            server=subprocess.Popen([sys.argv[1]],cwd=str(root),env=environment,stdout=log,stderr=subprocess.STDOUT)
            wait_server(port,server)
            features=call('query_capabilities',{})
            assert 'control_workers' not in features['operations']
            assert features['features']['remote_worker_management'] is False
            assert features['features']['record_deletion'] is False
            worker=subprocess.Popen([sys.executable,'-B','-u',str(Path(sys.argv[2]).resolve()/'worker/gcrp_worker.py'),
                '--worker-id','fixture-slot1','--machine-id','fixture','--slot-id','slot1',
                '--demo','--demo-seconds','2'],cwd=str(root),env=environment,stdout=log,stderr=subprocess.STDOUT)
            eventually(lambda: row() and row()['control_supported'])
            first=call('submit_task',{'task_type':'REGRESSION','target':'ordinary','revision':'demo'})['task_id']
            eventually(lambda: task(first)['status']=='RUNNING')
            queued=call('submit_task',{'task_type':'REGRESSION','target':'cancel-queued','revision':'demo'})['task_id']
            call('cancel_task',{'task_id':queued})
            assert task(queued)['status']=='CANCELLED'
            deny_all()
            eventually(lambda: task(first)['status']=='SUCCEEDED')
            stop(server)
            server=subprocess.Popen([sys.argv[1]],cwd=str(root),env=environment,stdout=log,stderr=subprocess.STDOUT)
            wait_server(port,server)
            eventually(lambda: row() and row()['status']=='ONLINE')
            deny_all()
            final=call('submit_task',{'task_type':'REGRESSION','target':'after-restart','revision':'demo'})['task_id']
            eventually(lambda: task(final)['status']=='SUCCEEDED')
            assert worker.poll() is None
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read()[-6000:],file=sys.stderr)
            raise
        finally:
            stop(worker)
            stop(server)
print('WORKER_CONTROL_PROCESS PASS: admin requests denied before/after restart; submit/query/queued cancel unaffected')
