"""Two logical machines with two independent slot processes each."""
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

from test_sqlite_worker import request, wait_server, stop


def eventually(check, seconds=25):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError('condition timed out')


def children(process):
    path = Path('/proc/{}/task/{}/children'.format(process.pid, process.pid))
    if not path.exists():
        return []
    return [int(pid) for pid in path.read_text().split()]


def slot_process(manager, slot):
    for pid in children(manager):
        try:
            args = Path('/proc/{}/cmdline'.format(pid)).read_bytes().split(b'\0')
            if b'--slot-id' in args and args[args.index(b'--slot-id') + 1] == slot.encode():
                return pid
        except (OSError, ValueError, IndexError):
            continue
    return None


def status(port):
    value = request(port, 'query_status', {})
    assert value['code'] == 0, value
    return value['result']


def task(port, task_id):
    value = request(port, 'query_task', dict(task_id=task_id))
    assert value['code'] == 0, value
    return value['result']


def main():
    server_bin, manager_script = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='forge-p13-') as directory:
        root = Path(directory)
        probe = __import__('socket').socket()
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
        probe.close()
        shared = ('server.bind_ip=127.0.0.1\nserver.port={}\n'
                  'network.server_ip=127.0.0.1\nserver.io_threads=2\n'
                  'server.worker_threads=4\nlog.level=WARN\nlog.dir={}\n'
                  'database.path={}\nworker.reconnect_seconds=0.1\n'
                  'server.worker_timeout_seconds=3\nworker.checkout_parallelism=1\n'
                  'worker.output_dir={}\n').format(
                      port, root / 'logs', root / 'state.db', root / 'legacy')
        server_conf = root / 'server.conf'
        server_conf.write_text(shared)
        manager_confs = {}
        def output_root(machine):
            return root / ('outputs-' + machine)

        def manager_config(machine, count):
            return shared + 'worker.output_root={}\nworker.slots={}\n'.format(
                output_root(machine), count)

        for machine in ('a', 'b'):
            path = root / (machine + '.conf')
            path.write_text(manager_config(machine, 2))
            manager_confs[machine] = path
        files = []
        processes = []
        orphans = []

        def start(args, config):
            output = (root / 'process-{}.log'.format(len(files))).open('wb')
            files.append(output)
            environment = dict(os.environ, FORGESCHED_CONFIG=str(config))
            if config in manager_confs.values():
                environment['FORGESCHED_MACHINE_NAME'] = config.stem
            process = subprocess.Popen(args, cwd=str(root),
                env=environment,
                stdout=output, stderr=subprocess.STDOUT)
            processes.append(process)
            return process

        def submit():
            reply = request(port, 'submit_task', dict(task_type='REGRESSION',
                target='parallel', revision='18925', payload={}))
            assert reply['code'] == 0, reply
            return reply['result']['task_id']

        def succeeded(ids):
            return all(task(port, task_id)['status'] == 'SUCCEEDED' for task_id in ids)

        managers = {}
        try:
            server = start([server_bin], server_conf)
            wait_server(port, server)
            for machine in ('a', 'b'):
                managers[machine] = start([sys.executable, '-B', '-u', manager_script,
                    '--demo', '--demo-seconds', '4.5'], manager_confs[machine])
            eventually(lambda: len(status(port)['workers']) == 4)
            snapshot = status(port)
            assert sorted(snapshot['machines']) == ['a', 'b'], snapshot
            assert snapshot['machines']['a']['slots'] == 2
            assert snapshot['machines']['b']['slots'] == 2
            identity_a = json.loads((output_root('a') / '.machine-identity.json').read_text())
            assert len({(w['machine_id'], w['slot_id']) for w in snapshot['workers']}) == 4
            for machine in ('a', 'b'):
                for slot in ('slot1', 'slot2'):
                    assert (output_root(machine) / machine / slot).is_dir()
                    assert slot_process(managers[machine], slot)
            duplicate = start([sys.executable, '-B', '-u', manager_script, '--demo'], manager_confs['a'])
            assert duplicate.wait(timeout=5) != 0

            ids = [submit() for _ in range(5)]
            eventually(lambda: all(task(port, task_id)['status'] == 'RUNNING' for task_id in ids[:4]))
            assert task(port, ids[4])['status'] == 'QUEUED'
            assert len({task(port, task_id)['worker_id'] for task_id in ids[:4]}) == 4
            assert eventually(lambda: succeeded(ids), 25)
            with sqlite3.connect(str(root / 'state.db')) as db:
                assert db.execute('SELECT count(*) FROM task_runs WHERE task_id BETWEEN ? AND ?',
                                  (ids[0], ids[-1])).fetchone()[0] == 5

            # One slot dies; the other three remain schedulable. Its guard must
            # finish cleanup before the manager restarts that slot and retries.
            fault = submit()
            eventually(lambda: task(port, fault)['status'] == 'RUNNING')
            original = task(port, fault)
            machine, slot = original['worker_id'].split('-')
            pid = eventually(lambda: slot_process(managers[machine], slot))
            journal = output_root(machine) / machine / slot
            eventually(lambda: list(journal.glob('run-*/{}/pid'.format(fault))))
            os.kill(pid, signal.SIGKILL)
            assert eventually(lambda: task(port, fault)['status'] == 'SUCCEEDED', 30)
            assert task(port, fault)['retry_count'] == 1
            assert task(port, fault)['run_id'] != original['run_id']
            eventually(lambda: slot_process(managers[machine], slot) not in (None, pid))

            # Removing one slot asks it to finish its current case. The fifth
            # queued case cannot be assigned to that draining slot.
            batch = [submit() for _ in range(5)]
            eventually(lambda: all(task(port, item)['status'] == 'RUNNING' for item in batch[:4]))
            assert task(port, batch[4])['status'] == 'QUEUED'
            assert any(task(port, item)['worker_id'] == 'a-slot2' for item in batch[:4])
            manager_confs['a'].write_text(manager_config('a', 1))
            os.kill(managers['a'].pid, signal.SIGHUP)
            eventually(lambda: any(w['worker_id'] == 'a-slot2' and
                w['status'] == 'DRAINING' for w in status(port)['workers']))
            assert eventually(lambda: succeeded(batch), 30)
            assert task(port, batch[4])['worker_id'] != 'a-slot2'
            eventually(lambda: any(w['worker_id'] == 'a-slot2' and
                w['status'] == 'OFFLINE' for w in status(port)['workers']))
            assert slot_process(managers['a'], 'slot2') is None

            manager_confs['a'].write_text(manager_config('a', 3))
            os.kill(managers['a'].pid, signal.SIGHUP)
            eventually(lambda: all(any(w['worker_id'] == 'a-' + slot and
                w['status'] == 'ONLINE' for w in status(port)['workers'])
                for slot in ('slot1', 'slot2', 'slot3')))
            assert json.loads((output_root('a') / '.machine-identity.json').read_text()) == identity_a
            assert slot_process(managers['a'], 'slot2')
            assert slot_process(managers['a'], 'slot3')

            # Parent manager death must not duplicate living slot processes.
            orphan_pids = children(managers['b'])
            assert len(orphan_pids) == 2
            managers['b'].kill()
            managers['b'].wait(timeout=5)
            orphans.extend(orphan_pids)
            managers['b'] = start([sys.executable, '-B', '-u', manager_script,
                '--demo', '--demo-seconds', '4.5'], manager_confs['b'])
            time.sleep(1)
            assert not children(managers['b'])
            assert all(Path('/proc/{}'.format(pid)).exists() for pid in orphan_pids)
            more = [submit() for _ in range(3)]
            assert eventually(lambda: succeeded(more), 25)
            for pid in orphan_pids:
                os.kill(pid, signal.SIGTERM)
            orphans.clear()
            eventually(lambda: len(children(managers['b'])) == 2, 15)
            with sqlite3.connect(str(root / 'state.db')) as db:
                assert db.execute('SELECT count(*) FROM task_runs').fetchone()[0] == len(ids) + 2 + len(batch) + len(more)
        except BaseException:
            for path in sorted(root.glob('process-*.log')):
                print(str(path) + '\n' + path.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            for pid in orphans:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            for process in reversed(processes):
                stop(process)
            for output in files:
                output.close()


if __name__ == '__main__':
    main()
