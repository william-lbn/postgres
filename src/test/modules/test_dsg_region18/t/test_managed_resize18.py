#!/usr/bin/env python3
"""PG18 required-admission/PM-gate 与真实Buffer伸缩组合；非Root/normal许可。"""
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import socket
import time
import unittest

spec = importlib.util.spec_from_file_location('managed_connection18', Path(__file__).with_name('test_connection18.py'))
connection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(connection)
work = connection.work


class ManagedResize(unittest.TestCase):
    def fact(self, name, value):
        target = os.environ.get('NEON_PG18_EVIDENCE')
        if target:
            Path(target, name + '.json').write_text(json.dumps(value, indent=2) + '\n')

    def cluster(self):
        p = connection.Cluster(gate=True, admission=True, lease=60000)
        self.addCleanup(p.close)
        p.start()
        p.sql('CREATE ROLE work18_user LOGIN')
        p.sql('SELECT test_dsg_resize18_prepare()')
        self.wait(p, lambda s: s['acknowledged'] and not s['admission_open'] and s['public_connections_blocked'])
        return p

    def connection(self, p):
        return json.loads(p.sql('SELECT test_dsg_connection18_snapshot()').stdout)

    def state(self, p):
        return json.loads(p.sql('SELECT test_dsg_resize18_state()').stdout)

    def wait(self, p, predicate, seconds=8):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            state = self.connection(p)
            if predicate(state):
                return state
            time.sleep(.02)
        self.fail('actual managed gate did not reach required state: ' + json.dumps(state))

    def gate(self, p, opened):
        p.sql('SELECT test_dsg_work18_admission(' + ('true' if opened else 'false') + ')')
        return self.wait(p, lambda s: s['acknowledged'] and s['requested_open'] == opened and s['applied_open'] == opened)

    def resize(self, p, action, target, revision, timeout=5000):
        generation = int(self.state(p)['generation'])
        return json.loads(p.sql(f"SELECT test_dsg_resize18_request('{action}',{target},{revision},{generation},{timeout})").stdout)

    def committed(self, receipt, target):
        self.assertIn(receipt['disposition'], (0, 1))
        self.assertTrue(receipt['receipt_has_result'])
        self.assertEqual(receipt['result_phase'], 2)
        self.assertEqual((receipt['low'], receipt['high'], receipt['result_high']), (target, target, target))
        self.assertFalse(receipt['normal_permit'])

    def wire(self, p):
        w = work.Wire(type('PublicSocket', (), {'sock': p.public})())
        self.addCleanup(w.close)
        return w

    def public_rejected(self, p):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        try:
            s.connect(str(p.public / '.s.PGSQL.55438'))
            try:
                self.assertEqual(s.recv(1), b'')
            except ConnectionResetError:
                pass
        finally:
            s.close()

    def business_data(self, p):
        p.sql('CREATE TABLE managed_resize18_data(i integer,v text);GRANT ALL ON managed_resize18_data TO work18_user')
        self.gate(p, True)
        w = self.wire(p)
        errors, _, _ = w.query("INSERT INTO managed_resize18_data SELECT i,repeat(md5(i::text),4) FROM generate_series(1,150000)i")
        self.assertFalse(errors, errors)
        closed = self.gate(p, False)
        quiet = self.wait(p, lambda s: s['foreground_idle'] and s['retained_transactions'] == 0)
        self.public_rejected(p)
        p.sql('CHECKPOINT')
        return {'closed': closed, 'quiet': quiet, 'ordinary_backend_pid': w.pid}

    def assert_data(self, p):
        got = p.sql('SELECT count(*),sum(i),min(length(v)),max(length(v)) FROM managed_resize18_data').stdout.strip()
        self.assertEqual(got, '150000|11250075000|128|128')

    def test_PG18_S_MANAGED_RESIZE_001_required_gate_actual_grow_shrink_regrow(self):
        p = self.cluster()
        start = p.snapshot()
        grow = self.resize(p, 'grow', 2048, 1)
        self.committed(grow, 2048)
        business = self.business_data(p)
        self.assert_data(p)
        shrink = self.resize(p, 'shrink', 512, 2)
        self.committed(shrink, 512)
        self.assertLess(int(shrink['current_main_arrays_upper']), int(grow['current_main_arrays_upper']))
        self.assert_data(p)
        regrow = self.resize(p, 'grow', 2048, 3)
        self.committed(regrow, 2048)
        self.assert_data(p)
        p.sql('CHECKPOINT')
        again = self.resize(p, 'shrink', 512, 4)
        self.committed(again, 512)
        final = p.snapshot()
        self.assertEqual([a['base'] for a in start['arrays']], [a['base'] for a in final['arrays']])
        self.assertEqual(int(p.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout), 2)
        self.assert_data(p)
        self.fact('managed-resize-required-gate-regrow', {'start': start, 'business': business, 'grow': grow, 'shrink': shrink, 'regrow': regrow, 'again': again, 'gate': self.connection(p), 'final': final, 'Source': False, 'normal': False})

    def test_PG18_S_MANAGED_RESIZE_002_closed_gate_real_tail_pin_busy_then_retry(self):
        p = self.cluster()
        self.committed(self.resize(p, 'grow', 2048, 1), 2048)
        self.business_data(p)
        before = self.state(p)
        holder = p.asynchronous("SELECT test_dsg_resize18_hold_tail('managed_resize18_data',2000)")
        self.addCleanup(lambda: holder.kill() if holder.poll() is None else None)
        until = time.monotonic() + 4
        while time.monotonic() < until:
            pinned = self.state(p)
            if pinned['tail_now']['pinned'] > 0:
                break
            time.sleep(.02)
        else:
            self.fail('actual tail pin was not observed')
        denied = self.resize(p, 'shrink', 512, 2, timeout=80)
        self.assertTrue(denied['receipt_has_result'])
        self.assertEqual(denied['result_phase'], 3)
        self.assertEqual(denied['high'], 2048)
        self.assertEqual(denied['generation'], before['generation'])
        self.assertGreater(denied['drain_counts'][1], 0)
        self.assertGreaterEqual(int(denied['current_main_arrays_upper']), int(before['current_main_arrays_upper']))
        out, err = holder.communicate(timeout=8)
        self.assertEqual(holder.returncode, 0, err)
        self.assertGreater(int(out.strip()), 512)
        self.assertTrue(self.connection(p)['public_connections_blocked'])
        retry = self.resize(p, 'shrink', 512, 3)
        self.committed(retry, 512)
        self.assert_data(p)
        self.fact('managed-resize-real-busy-pin', {'before': before, 'pinned': pinned, 'denied': denied, 'retry': retry, 'held_buffer_id': int(out), 'Source': False, 'normal': False})

    def test_PG18_S_MANAGED_RESIZE_003_same_PM_crash_newboot_closed_then_resize(self):
        p = self.cluster()
        self.committed(self.resize(p, 'grow', 2048, 1), 2048)
        self.business_data(p)
        p.sql('CHECKPOINT')
        before = p.snapshot()
        old = self.gate(p, True)
        w = self.wire(p)
        self.assertFalse(w.query('BEGIN')[0])
        self.wait(p, lambda s: s['retained_transactions'] == 1)
        pm = p.process.pid
        pidfd = os.pidfd_open(w.pid)
        try:
            signal.pidfd_send_signal(pidfd, signal.SIGKILL)
            poll = select.poll()
            poll.register(pidfd, select.POLLIN)
            self.assertTrue(poll.poll(8000))
        finally:
            os.close(pidfd)
        w.close()
        p.ready()
        self.assertEqual(p.process.pid, pm)
        fresh = self.wait(p, lambda s: s['boot'] != old['boot'] and s['acknowledged'] and s['public_connections_blocked'])
        self.assertFalse(fresh['admission_open'])
        self.assertEqual((fresh['startup_clients'], fresh['client_processes'], fresh['retained_transactions']), (0, 0, 0))
        self.public_rejected(p)
        initial = self.state(p)
        self.assertEqual((initial['high'], int(initial['generation'])), (512, 1))
        self.assertEqual([a['base'] for a in before['arrays']], [a['base'] for a in p.snapshot()['arrays']])
        self.assert_data(p)
        # 新boot必须明确重新fence/prepare；这不是Root跨boot许可，也不冒旧boot请求负例。
        p.sql('SELECT test_dsg_resize18_prepare()')
        self.wait(p, lambda s: s['acknowledged'] and s['public_connections_blocked'])
        grown = self.resize(p, 'grow', 2048, 1)
        self.committed(grown, 2048)
        shrunk = self.resize(p, 'shrink', 512, 2)
        self.committed(shrunk, 512)
        regrown = self.resize(p, 'grow', 1536, 3)
        self.committed(regrown, 1536)
        self.assert_data(p)
        self.fact('managed-resize-same-PM-newboot', {'same_PM_PID': pm, 'old': old, 'fresh_closed': fresh, 'new_boot_initial': initial, 'grow': grown, 'shrink': shrunk, 'regrow': regrown, 'old_boot_exact_request_tested': False, 'Root_recovery_qualification': False, 'Source': False, 'normal': False})


if __name__ == '__main__':
    unittest.main(verbosity=2)
