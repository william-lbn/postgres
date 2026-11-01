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


