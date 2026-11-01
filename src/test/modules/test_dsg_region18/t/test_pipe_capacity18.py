#!/usr/bin/env python3
"""PG18准确filter的固定pipe容量组件；不是原Source或normal许可。"""
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import struct
import time
import unittest

import test_filter18 as filtered
import test_root_birth18 as rootwire

F_SETPIPE_SZ, F_GETPIPE_SZ = 1031, 1032
EVIDENCE = Path(os.environ['NEON_PG18_EVIDENCE'])
libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long


def syscall_fcntl(fd, command, value=0):
    ctypes.set_errno(0)
    result = libc.syscall(ctypes.c_long(72), ctypes.c_long(fd), ctypes.c_long(command), ctypes.c_long(value))
    return int(result), ctypes.get_errno() if result < 0 else 0


class SockFilter(ctypes.Structure):
    _fields_ = [('code', ctypes.c_ushort), ('jt', ctypes.c_ubyte), ('jf', ctypes.c_ubyte), ('k', ctypes.c_uint)]


class SockProgram(ctypes.Structure):
    _fields_ = [('length', ctypes.c_ushort), ('filter', ctypes.POINTER(SockFilter))]


def kernel_worker(encoded, output):
    """只对自己新建的pipe和当前专用子进程安装准确程序，不碰任何现有服务。"""
    read_end, write_end = os.pipe2(os.O_CLOEXEC)
    initial = fcntl.fcntl(read_end, F_SETPIPE_SZ, 4096)
    assert initial == 4096
    # 模拟原Root预置后作为PG的FD2继承；没有在filter后补改容量。
    os.dup2(write_end, 2, inheritable=True)
    before_positive = [syscall_fcntl(read_end, c, initial) for c in (F_SETPIPE_SZ, (1 << 32) | F_SETPIPE_SZ)]
    assert before_positive == [(initial, 0), (initial, 0)], before_positive
    rows = [struct.unpack_from('>HBBI', encoded, i) for i in range(0, len(encoded), 8)]
    instructions = (SockFilter * len(rows))(*[SockFilter(*row) for row in rows])
    program = SockProgram(len(rows), instructions)
    os.setgroups([])
    os.setgid(1000)
    os.setuid(1000)
    assert libc.prctl(38, 1, 0, 0, 0) == 0
    unprivileged_positive = [syscall_fcntl(read_end, c, initial) for c in (F_SETPIPE_SZ, (1 << 32) | F_SETPIPE_SZ)]
    assert unprivileged_positive == [(initial, 0), (initial, 0)], unprivileged_positive
    assert libc.prctl(22, 2, ctypes.byref(program), 0, 0) == 0
    denied = []
    for command in (F_SETPIPE_SZ, (1 << 32) | F_SETPIPE_SZ):
        for amount in (initial, initial * 2, 1):
            got = syscall_fcntl(read_end, command, amount)
            assert got == (-1, errno.EPERM), (command, amount, got)
            denied.append({'command': command, 'requested': amount, 'result': got})
    assert syscall_fcntl(2, F_SETPIPE_SZ, initial) == (-1, errno.EPERM)
    after = fcntl.fcntl(read_end, F_GETPIPE_SZ)
    assert after == initial
    assert syscall_fcntl(read_end, (1 << 32) | F_GETPIPE_SZ) == (initial, 0)
    assert syscall_fcntl(-1, F_SETPIPE_SZ, initial) == (-1, errno.EPERM)
    assert syscall_fcntl(-1, F_GETPIPE_SZ) == (-1, errno.EBADF)
    flags = fcntl.fcntl(read_end, fcntl.F_GETFL)
    assert fcntl.fcntl(read_end, fcntl.F_SETFL, flags | os.O_NONBLOCK) == 0
    assert fcntl.fcntl(read_end, fcntl.F_GETFL) & os.O_NONBLOCK
    assert fcntl.fcntl(read_end, fcntl.F_SETFD, fcntl.FD_CLOEXEC) == 0
    assert fcntl.fcntl(read_end, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
    duplicate = fcntl.fcntl(read_end, fcntl.F_DUPFD_CLOEXEC, 3)
    os.close(duplicate)
    assert syscall_fcntl(read_end, fcntl.F_SETFL, os.O_DIRECT) == (-1, errno.EPERM)
    assert os.write(write_end, b'p') == 1 and os.read(read_end, 1) == b'p'
    os.close(read_end)
    os.close(write_end)
    assert syscall_fcntl(read_end, fcntl.F_GETFD) == (-1, errno.EBADF)
    status = Path('/proc/self/status').read_text()
    assert '\nNoNewPrivs:\t1\n' in status and '\nSeccomp:\t2\n' in status
    fact = {'pid': os.getpid(), 'uid': os.getresuid(), 'gid': os.getresgid(),
            'before_positive': before_positive, 'unprivileged_NNP_preinstall_positive': unprivileged_positive, 'capacity_initial': initial, 'capacity_after': after,
            'denied': denied, 'normal_fcntl_roundtrip': True, 'O_DIRECT_still_denied': True,
            'program_sha256': hashlib.sha256(encoded).hexdigest(), 'Source': False, 'normal': False}
    raw = json.dumps(fact).encode()
    assert len(raw) < 4096 and os.write(output, raw) == len(raw)


def run_kernel_worker(encoded):
    reader, writer = os.pipe2(os.O_CLOEXEC)
    pid = os.fork()
    if pid == 0:
        os.close(reader)
        try:
            kernel_worker(encoded, writer)
        except BaseException as exc:
            raw = json.dumps({'error': repr(exc)}).encode()[:2048]
            os.write(writer, raw)
            os._exit(71)
        os._exit(0)
    os.close(writer)
    original = os.pidfd_open(pid, 0)  # 未wait/reap的本子进程，不存在PID复用收养。
    waited = False
    try:
        data = bytearray()
        deadline = time.monotonic() + 8
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([reader], [], [], left)[0]:
                raise AssertionError('dedicated kernel worker bounded result timeout')
            raw = os.read(reader, 4096)
            if not raw:
                break
            data.extend(raw)
            assert len(data) < 8192
        actual, status = os.waitpid(pid, 0)
        waited = True
        assert actual == pid and os.waitstatus_to_exitcode(status) == 0, (status, data)
        assert select.select([original], [], [], 0)[0]
        result = json.loads(data)
        result['actual_unique_Wait_exit'] = 0
        return result
    finally:
        if not waited:
            signal.pidfd_send_signal(original, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(original)
        os.close(reader)


class Pipe18(unittest.TestCase):
    def save(self, name, value):
        EVIDENCE.mkdir(parents=True, exist_ok=True)
        (EVIDENCE / (name + '.json')).write_text(json.dumps(value, indent=2) + '\n')

    def test_PG18_S_PIPE_001_actual_all_three_kernel_programs(self):
        audit = rootwire.audit()
        self.assertEqual(audit.returncode, 0, audit.stderr)
        programs = rootwire.decode_bundle(audit.stdout)
        self.assertEqual(len(programs), 3)
        evidence = []
        for name, program in zip(('PMbase', 'PMfinal', 'child'), programs):
            with self.subTest(stage=name):
                result = run_kernel_worker(program)
                result['stage'] = name
                evidence.append(result)
        self.save('actual-pipe-three-kernel-programs', evidence)

    def assert_normal_fcntl(self, result):
        self.assertGreater(result['pipe_capacity_before'], 0, result)
        for name in ('nonblock', 'cloexec', 'roundtrip', 'closed'):
            self.assertTrue(result[name], result)
        self.assertEqual((result['setfl'], result['setfd'], result['dup_errno']), (0, 0, 0), result)
        self.assertEqual(result['bad_get_errno'], errno.EBADF, result)

    def test_PG18_S_PIPE_002_real_postmaster_backend_and_default_off(self):
        evidence = []
        for enabled in (False, True):
            with self.subTest(filtered=enabled):
                pg = filtered.Cluster(filtered=enabled, probe='pipe-capacity-v1' if enabled else None)
                self.addCleanup(pg.close)
                pg.start()
                result = json.loads(pg.sql("SELECT test_dsg_filter_probe18('pipe-capacity')").stdout)
                self.assert_normal_fcntl(result)
                self.assertGreater(result['stderr_capacity_before'], 0, result)  # 真实PG collector pipe FD2。
                if enabled:
                    self.assertEqual(result['stderr_same_result'], -1, result)
                    self.assertEqual(result['stderr_same_errno'], errno.EPERM, result)
                    self.assertEqual(result['results'], [-1] * 3, result)
                    self.assertEqual(result['errors'], [errno.EPERM] * 3, result)
                    self.assertEqual(result['pipe_capacity_after'], result['pipe_capacity_before'], result)
                    self.assertEqual(result['bad_set_errno'], errno.EPERM, result)
                    marker = 'DSGA_FILTER_PM_PIPE_CAPACITY '
                    pm = [json.loads(line.split(marker, 1)[1]) for line in pg.logs().splitlines() if marker in line]
                    self.assertEqual(len(pm), 1, pg.logs())
                    self.assert_normal_fcntl(pm[0])
                    self.assertEqual(pm[0]['pid'], pg.process.pid)
                    self.assertEqual(pm[0]['results'], [-1] * 3, pm)
                    self.assertEqual(pm[0]['errors'], [errno.EPERM] * 3, pm)
                    self.assertEqual(pm[0]['pipe_capacity_after'], pm[0]['pipe_capacity_before'], pm)
                    result['actual_PMfinal'] = pm[0]
                    original = json.loads(pg.sql('SELECT test_dsg_birth_filter18()').stdout)
                    self.assertTrue(all(original[k] for k in ('enabled', 'postmaster_installed', 'finalized', 'child_installed')), original)
                    self.assertFalse(original['root_enrolled'])
                    result['actual_filter'] = original
                    direct = json.loads(pg.sql("SELECT test_dsg_filter_probe18('direct-fcntl')").stdout)
                    self.assertEqual((direct['result'], direct['errno']), (-1, errno.EPERM), direct)
                else:
                    self.assertEqual(result['stderr_same_result'], result['stderr_capacity_before'], result)
                    self.assertEqual(result['stderr_same_errno'], 0, result)
                    self.assertEqual(result['results'][0], result['pipe_capacity_before'], result)
                    self.assertGreater(result['results'][1], 0, result)
                    self.assertEqual(result['errors'][:2], [0, 0], result)
                    self.assertEqual(result['bad_set_errno'], errno.EBADF, result)
                    self.assertNotEqual(pg.sql("SELECT test_dsg_filter_probe18('private-outside')", check=False).returncode, 0)
                pg.sql('CREATE ROLE pipe18_ordinary LOGIN; GRANT EXECUTE ON FUNCTION test_dsg_filter_probe18(text) TO pipe18_ordinary')
                rejected = pg.sql("SET ROLE pipe18_ordinary;SELECT test_dsg_filter_probe18('pipe-capacity')", check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn('native filter probes require original administrator', rejected.stderr)
                pg.sql('CREATE TABLE pipe18_data AS SELECT i,md5(i::text) v FROM generate_series(1,1000)i;CHECKPOINT')
                self.assertEqual(pg.sql('SELECT count(*),sum(i) FROM pipe18_data').stdout.strip(), '1000|500500')
                self.assertEqual(pg.sql('SHOW io_method').stdout.strip(), 'worker')
                self.assertEqual(int(pg.sql("SELECT count(*) FROM pg_stat_activity WHERE backend_type='io worker'").stdout), 2)
                result['logs'] = pg.logs()
                pg.close()
                self.assertEqual(pg.process.returncode, 0)
                result['actual_unique_PM_Wait_exit'] = pg.process.returncode
                result['filtered'] = enabled
                result['Source'] = result['normal'] = False
                evidence.append(result)
        self.save('actual-PG-pipe-capacity-default-and-protected', evidence)


if __name__ == '__main__':
    unittest.main(verbosity=2)
