#!/usr/bin/env python3
"""Safety checks for automatic machine identity and legacy SVN reuse."""
import fcntl
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'worker'))
from slot_identity import machine_identity, migrate_legacy_slots, slot_names

def checkout(path):
    (path / 'galaxcore/.svn').mkdir(parents=True)
    (path / 'galaxcore/test2').mkdir()
    (path / 'galaxcore/test2/run.sh').write_text('#!/bin/sh\n')

def main():
    with tempfile.TemporaryDirectory(prefix='gcrp-slot-identity-') as temporary:
        root = Path(temporary)
        output = root / 'output'
        slots = root / 'slots'
        slots.mkdir()
        previous = os.environ.get('GCRP_MACHINE_NAME')
        try:
            os.environ['GCRP_MACHINE_NAME'] = 'novarch4'
            identity = machine_identity(output)
            assert identity['name'] == 'novarch4'
            os.environ['GCRP_MACHINE_NAME'] = 'changed-hostname'
            assert machine_identity(output) == identity
        finally:
            if previous is None:
                del os.environ['GCRP_MACHINE_NAME']
            else:
                os.environ['GCRP_MACHINE_NAME'] = previous
        assert slot_names(3) == ['slot1', 'slot2', 'slot3']
        for index in (1, 2):
            checkout(slots / 'novarch4-slot-01-slot-0{}'.format(index))
        plan = migrate_legacy_slots(slots, output, 'novarch4', slot_names(2), preview=True)
        assert len(plan) == 2
        assert not (slots / 'novarch4-slot1').exists()
        migrate_legacy_slots(slots, output, 'novarch4', slot_names(2))
        assert all((slots / ('novarch4-slot' + str(index)) /
                    'galaxcore/.svn').is_dir() for index in (1, 2))
        assert not (slots / 'novarch4-slot-01-slot-01').exists()
        assert migrate_legacy_slots(slots, output, 'novarch4', slot_names(2)) == []
        checkout(slots / 'novarch4-slot-01-slot-03')
        legacy_output = output / 'novarch4-slot-01' / 'slot-03'
        legacy_output.mkdir(parents=True)
        (legacy_output / 'pending-report-test.json').write_text('{}')
        try:
            migrate_legacy_slots(slots, output, 'novarch4', slot_names(3))
            raise AssertionError('pending report must block migration')
        except RuntimeError as error:
            assert 'unresolved task state' in str(error)
        assert (slots / 'novarch4-slot-01-slot-03').exists()
        (legacy_output / 'pending-report-test.json').unlink()
        manager_lock = output / 'novarch4-slot-01' / '.manager.lock'
        manager_lock.touch()
        with manager_lock.open('r+') as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                migrate_legacy_slots(slots, output, 'novarch4', slot_names(3))
                raise AssertionError('live old manager must block migration')
            except RuntimeError as error:
                assert 'still owned' in str(error)
        migrate_legacy_slots(slots, output, 'novarch4', slot_names(3))
        assert (slots / 'novarch4-slot3/galaxcore/test2/run.sh').is_file()
        assert identity['uid'] == machine_identity(output)['uid']

if __name__ == '__main__':
    main()
