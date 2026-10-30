#!/usr/bin/env python3
"""真实Linux原生Root wire/ptrace组件；仅隔离fixture，不是生产Root或normal资格。"""
import array
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import select
import socket
import struct
import subprocess
import threading
import tempfile
import time
import unittest

BIN = Path(os.environ['NEON_TEST_PG_BIN']) / 'postgres'
EVIDENCE = Path(os.environ['NEON_ROOT_WIRE_EVIDENCE'])
SOCKET = '/run/neon-dsga/birth.sock'
libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long
SEIZE, INTERRUPT, CONT, DETACH, GET_FILTER = 0x4206, 0x4207, 7, 17, 0x420c
EXITKILL = 1 << 20
PREFIX = 'NEON_DSGA_'
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith(PREFIX) and not k.startswith('LD_')}
BASE_ENV.update({
    PREFIX+'EARLY_ARRAYS_PROFILE': 'root-v1:8192',
    PREFIX+'EARLY_ARRAYS_FILTER': 'root-v1',
    PREFIX+'ADDRESS_SPACE_MAX_BYTES': str(2 << 30),
    PREFIX+'NATIVE_BIRTH_NONCE': '12'*32,
    PREFIX+'NATIVE_BIRTH_SOURCE_SHA256': '34'*32,
    PREFIX+'NATIVE_BIRTH_PROFILE_SHA256': '56'*32,
    PREFIX+'NATIVE_BIRTH_STARTUP_BUFFERS': '256',
})


def ptrace(op, pid, addr=0, data=0):
    ctypes.set_errno(0)
    r = libc.ptrace(ctypes.c_ulong(op), ctypes.c_ulong(pid), ctypes.c_void_p(addr), ctypes.c_void_p(data))
    if r == -1:
        raise OSError(ctypes.get_errno(), f'ptrace {op:x} pid={pid}')
    return r


def unprivileged(occupied=False):
    os.setgroups([])
    os.setgid(1000)
    os.setuid(1000)
    if libc.prctl(38, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_SET_NO_NEW_PRIVS')
    if occupied:
        fd = os.open('/dev/null', os.O_RDONLY)
        os.dup2(fd, 198, inheritable=True)
        if fd != 198:
            os.close(fd)


def wait_stop(pid, signum, timeout=5):
    until = time.monotonic()+timeout
    while time.monotonic() < until:
        waited, status = os.waitpid(pid, os.WNOHANG | 0x40000000)
        if waited:
            if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signum:
                raise AssertionError(f'unexpected actual trace status {status:x}, expected {signum}')
            return status
        time.sleep(.005)
    raise AssertionError('bounded wait for real ptrace stop expired')


def get_filters(pid):
    out = []
    for i in range(5):
        try:
            n = ptrace(GET_FILTER, pid, i, 0)
        except OSError as e:
            if e.errno == errno.ENOENT:
                return out
            raise
        assert 0 < n <= 1024, n
        raw = ctypes.create_string_buffer(8*n)
        assert ptrace(GET_FILTER, pid, i, ctypes.addressof(raw)) == n
        native = [struct.unpack_from('=HBBI', raw.raw, 8*j) for j in range(n)]
        out.append(b''.join(struct.pack('>HBBI', *row) for row in native))
    raise AssertionError('unexpected extra filter beyond exact expected chain')


def audit(request=None, args=None, uid0=True):
    if args is None:
        if request is None:
            args = [str(2 << 30), '1', str(0x10000000), '4096', str(0x20000000), '8192',
                    str(0x30000000), '4096', str(0x40000000), '4096']
        else:
            limit, maximum = struct.unpack_from('>QI', request, 112)
            args = [str(limit), str(maximum)]
            for i in range(4):
                base, length, _ = struct.unpack_from('>QQQ', request, 136+24*i)
                args.extend([str(base), str(length)])
    env = {k: v for k, v in os.environ.items() if not k.startswith(PREFIX) and not k.startswith('LD_')}
    return subprocess.run([str(BIN), '--neon-dsga-audit-filter-v1', *args], env=env,
                          preexec_fn=None if uid0 else unprivileged,
                          capture_output=True, timeout=8)


def decode_bundle(raw, request=None):
    assert raw[:8] == b'NDSGFP01' and len(raw) >= 144
    assert struct.unpack_from('>IIII', raw, 8) == (1, 0xc000003e, 3, 0)
    if request is not None:
        assert raw[24:48] == request[112:136]
        assert raw[48:144] == request[136:232]
    at = 144
    programs = []
    for kind in (1, 2, 3):
        got, count = struct.unpack_from('>II', raw, at)
        assert got == kind and 0 < count <= 1024
        at += 8
        program = raw[at:at+8*count]
        assert len(program) == 8*count
        programs.append(program)
        at += 8*count
    assert at == len(raw), 'bundle trailing or missing bytes'
    return programs


class RootBirth(unittest.TestCase):
    def setUp(self):
        self.assertEqual(os.environ.get('NEON_DSGA_ROOT_COMPONENT_TEST'), '1')
        self.assertEqual(os.getuid(), 0)
        self.assertEqual(libc.prctl(21, 0, 0, 0, 0), 0, 'fixture Root must have Seccomp=0')
        status = Path('/proc/self/status').read_text()
        cap = int(next(x.split()[1] for x in status.splitlines() if x.startswith('CapEff:')), 16)
        self.assertEqual(cap & ((1 << 21) | (1 << 19)), (1 << 21) | (1 << 19))
        Path(SOCKET).parent.mkdir(parents=True, exist_ok=True)
        Path(SOCKET).parent.chmod(0o755)
        self.peers = []
        self.children = []
        self.root_helpers = []
        self.facts = []

    def tearDown(self):
        # fixture自身的root peer/tracer也须同步回收；不能因目标失败遗留tracer。
        for pid in list(self.root_helpers):
            waited, status = os.waitpid(pid, os.WNOHANG)
            if not waited:
                os.kill(pid, signal.SIGKILL)
                waited, status = os.waitpid(pid, 0)
            self.assertEqual(waited, pid)
            self.root_helpers.remove(pid)
        for p in self.children:
            if p.poll() is None:
                p.kill()
                try:
                    p.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    # 若出现trace stop，Root必须显式杀并严格wait，绝不忽略孤儿。
                    ptrace(CONT, p.pid, 0, signal.SIGKILL)
                    p.communicate(timeout=5)
        for peer in self.peers:
            peer.close()
        Path(SOCKET).unlink(missing_ok=True)
        (EVIDENCE/(self._testMethodName+'.json')).write_text(json.dumps(self.facts, indent=2)+'\n')

    def owned_fork(self):
        pid = os.fork()
        if pid > 0:
            self.root_helpers.append(pid)
        return pid

    def reap_helper(self, pid):
        until = time.monotonic()+8
        while time.monotonic() < until:
            waited, status = os.waitpid(pid, os.WNOHANG)
            if waited:
                self.root_helpers.remove(pid)
                self.assertEqual(waited, pid)
                return status
            time.sleep(.005)
        self.fail('owned Root fixture helper did not synchronously exit')

    def child(self, changes=None, occupied=False, args=('--version',)):
        env = dict(BASE_ENV)
        for k, v in (changes or {}).items():
            if v is None:
                env.pop(PREFIX+k, None)
            else:
                env[PREFIX+k] = v
        if occupied:
            try:
                os.fstat(198)
            except OSError as exc:
                self.assertEqual(exc.errno, errno.EBADF)
            else:
                self.fail('fixture must not overwrite parent fd198')
            fd = os.open('/dev/null', os.O_RDONLY)
            os.dup2(fd, 198, inheritable=True)
            if fd != 198:
                os.close(fd)
        try:
            p = subprocess.Popen([str(BIN), *map(str, args)], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, preexec_fn=unprivileged,
                                 pass_fds=(198,) if occupied else ())
        finally:
            if occupied:
                os.close(198)
        self.children.append(p)
        return p

    def server(self, nonroot=False):
        Path(SOCKET).unlink(missing_ok=True)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        if nonroot:
            Path(SOCKET).parent.chmod(0o777)
            os.seteuid(1000)
        try:
            s.bind(SOCKET)
            os.chmod(SOCKET, 0o666)
            s.listen(1)
        finally:
            if nonroot:
                os.seteuid(0)
        s.settimeout(5)
        self.peers.append(s)
        return s

    def exchange(self, mode='detach', threaded=False, args=('--version',)):
        s = self.server()
        p = self.child(args=args)
        c, _ = s.accept()
        self.peers.append(c)
        c.settimeout(5)
        request, controls, flags, _ = c.recvmsg(233, 1024)
        self.assertEqual(len(request), 232)
        self.assertEqual(controls, [])
        self.assertFalse(flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC))
        self.assertEqual(request[:16], b'NDSGBR01'+struct.pack('>II', 1, 1))
        expected = audit(request)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        programs = decode_bundle(expected.stdout, request)
        self.facts.append({'mode': mode, 'requestHex': request.hex(),
                           'expectedBundleHex': expected.stdout.hex(), 'threadedTracer': threaded})

        def trace():
            if mode != 'no_tracer':
                ptrace(SEIZE, p.pid, 0, EXITKILL)
                ptrace(INTERRUPT, p.pid)
                base_status = wait_stop(p.pid, signal.SIGTRAP)
                self.assertEqual(get_filters(p.pid), programs[:1])
                status = Path(f'/proc/{p.pid}/status').read_text()
                self.assertIn('NoNewPrivs:\t1', status)
                limits = Path(f'/proc/{p.pid}/limits').read_text()
                self.assertIn('2147483648', limits)
                maps = Path(f'/proc/{p.pid}/maps').read_text()
                for i in range(4):
                    base, length, _ = struct.unpack_from('>QQQ', request, 136+24*i)
                    spans = [(int(x.split()[0].split('-')[0], 16), int(x.split()[0].split('-')[1], 16), x.split()[1]) for x in maps.splitlines()]
                    self.assertTrue(any(a <= base and base+length <= b and perm == '---p' for a, b, perm in spans))
                ptrace(CONT, p.pid)
                self.facts[-1]['baseStopStatus'] = base_status
            ack = bytearray(request[:112])
            ack[12:16] = struct.pack('>I', 2)
            c.send(ack)
            if mode == 'no_tracer':
                return
            stopped = wait_stop(p.pid, signal.SIGSTOP)
            self.assertEqual(stopped >> 16, 0, 'must be signal-delivery stop, not group/event stop')
            self.assertEqual(get_filters(p.pid), programs[:2])
            self.assertFalse(Path(f'/proc/{p.pid}/fd/198').exists())
            status = Path(f'/proc/{p.pid}/status').read_text()
            tracer = int(next(x.split()[1] for x in status.splitlines() if x.startswith('TracerPid:')))
            self.assertEqual(tracer, threading.get_native_id())
            if threaded:
                self.assertNotEqual(tracer, os.getpid(), 'exercise TGID != tracer TID')
            self.facts[-1].update({'finalStopStatus': stopped, 'rootTGID': os.getpid(), 'tracerTID': tracer,
                                  'exactKernelFiltersMatched': 2, 'fixed198Closed': True})
            if mode == 'early_sigcont':
                sender = self.owned_fork()
                if sender == 0:
                    unprivileged()
                    os.kill(p.pid, signal.SIGCONT)
                    os._exit(0)
                status = self.reap_helper(sender)
                self.assertEqual(status, 0)
                time.sleep(.1)
                status = Path(f'/proc/{p.pid}/status').read_text()
                self.assertIn('tracing stop', status)
                self.assertIsNone(p.poll(), 'same-uid SIGCONT must not release Root ptrace stop')
                self.facts[-1]['sameUIDSignalCannotRelease'] = True
                ptrace(CONT, p.pid, 0, signal.SIGKILL)
            elif mode == 'cont_only':
                ptrace(CONT, p.pid)
            else:
                ptrace(DETACH, p.pid)

        def trace_with_evidence():
            try:
                trace()
            except BaseException:
                if p.poll() is None:
                    p.kill()
                out, err = p.communicate(timeout=5)
                self.facts[-1].update({'traceFailureStdout': out.decode(errors='replace'),
                                      'traceFailureStderr': err.decode(errors='replace')})
                print('actual native failure stderr:', err.decode(errors='replace'), flush=True)
                raise

        if threaded:
            failures = []
            def run():
                try:
                    trace_with_evidence()
                except BaseException as exc:
                    failures.append(exc)
            thread = threading.Thread(target=run)
            thread.start()
            thread.join(15)
            self.assertFalse(thread.is_alive(), 'Root trace thread must synchronously finish')
            if failures:
                raise failures[0]
        else:
            trace_with_evidence()
        stdout, stderr = p.communicate(timeout=8)
        self.facts[-1].update({'returncode': p.returncode, 'stdout': stdout.decode(errors='replace'),
                              'stderr': stderr.decode(errors='replace')})
        return p.returncode, stdout, stderr

    def test_PG17_U_ROOTWIRE_001_audit_exact_root_geometry(self):
        good = audit()
        self.assertEqual(good.returncode, 0, good.stderr)
        programs = decode_bundle(good.stdout)
        self.assertEqual(len(programs), 3)
        self.assertEqual(audit().stdout, good.stdout)
        self.assertEqual(audit(uid0=False).returncode, 1)
        args = [str(2 << 30), '1', str(0x10000000), '4096', str(0x20000000), '8192', str(0x30000000), '4096', str(0x40000000), '4096']
        cases = [args[:-1], args+['1'], ['02147483648']+args[1:], args[:1]+['0']+args[2:],
                 args[:2]+[str(0x10000001)]+args[3:], args[:3]+['8192']+args[4:],
                 args[:4]+[str(0x10000000)]+args[5:], ['65536']+args[1:]]
        # 65536仍大于M=1总R，改为page对齐且A不大于R的几何。
        cases[-1] = ['65536', '8', str(0x10000000), '4096', str(0x20000000), '65536', str(0x30000000), '4096', str(0x40000000), '4096']
        for argv in cases:
            with self.subTest(argv=argv):
                r = audit(args=argv)
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertEqual(r.stdout, b'')
        self.facts.append({'deterministicCompletePrograms': True, 'instructionCounts': [len(x)//8 for x in programs], 'negativeCases': len(cases)+1})

    def test_PG17_S_ROOTWIRE_001_real_exact_chain_detach(self):
        for threaded in (False, True):
            with self.subTest(threaded=threaded):
                rc, out, err = self.exchange(threaded=threaded)
                self.assertEqual(rc, 0, err)
                self.assertIn(b'postgres (PostgreSQL)', out)

    def test_PG17_S_ROOTWIRE_002_no_tracer_and_cont_cannot_release(self):
        for mode, message in [('no_tracer', b'requires original Root tracer'), ('cont_only', b'only original Root detach')]:
            with self.subTest(mode=mode):
                rc, out, err = self.exchange(mode)
                self.assertEqual(rc, 1)
                self.assertEqual(out, b'')
                self.assertIn(message, err)

    def test_PG17_S_ROOTWIRE_003_same_uid_sigcont_cannot_release(self):
        rc, out, err = self.exchange('early_sigcont')
        self.assertEqual(rc, -signal.SIGKILL)
        self.assertEqual(out, b'')

    def test_PG17_U_ROOTWIRE_002_invalid_original_fields_and_198(self):
        cases = [{'NATIVE_BIRTH_NONCE': None}, {'NATIVE_BIRTH_NONCE': '0'*64},
                 {'NATIVE_BIRTH_NONCE': 'A'*64}, {'NATIVE_BIRTH_SOURCE_SHA256': '0'*64},
                 {'NATIVE_BIRTH_PROFILE_SHA256': '00'}, {'NATIVE_BIRTH_STARTUP_BUFFERS': '8193'},
                 {'NATIVE_BIRTH_STARTUP_BUFFERS': '0256'}, {'EARLY_ARRAYS_FILTER': 'test-v1'},
                 {'EARLY_ARRAYS_PROFILE': 'test-v1:8192'}]
        for fields in cases:
            with self.subTest(fields=fields):
                p = self.child(fields)
                out, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(out, b'')
                self.assertIn(b'DSGA native birth wire:', err)
        p = self.child(occupied=True)
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 1)
        self.assertIn(b'reserved birth descriptor was already open', err)
        self.facts.append({'strictOriginalInputNegatives': len(cases), 'fixed198Conflict': True})

    def test_PG17_U_ROOTWIRE_003_ack_frames_ancillary_and_peer(self):
        for mode in ('short', 'long', 'nonce', 'source', 'profile', 'magic', 'rights', 'truncated_rights', 'eof'):
            with self.subTest(mode=mode):
                s = self.server()
                p = self.child()
                c, _ = s.accept()
                self.peers.append(c)
                c.settimeout(5)
                request = c.recv(233)
                self.assertEqual(len(request), 232)
                ack = bytearray(request[:112])
                ack[12:16] = struct.pack('>I', 2)
                if mode == 'short': ack = ack[:-1]
                elif mode == 'long': ack += b'x'
                elif mode in ('nonce', 'source', 'profile', 'magic'):
                    at = {'nonce': 16, 'source': 48, 'profile': 80, 'magic': 0}[mode]
                    ack[at] ^= 1
                if mode in ('rights', 'truncated_rights'):
                    fd = os.open('/dev/null', os.O_RDONLY)
                    try:
                        fds = array.array('i', [fd] * (1 if mode == 'rights' else 48))
                        c.sendmsg([ack], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)])
                    finally:
                        os.close(fd)
                elif mode == 'eof':
                    c.shutdown(socket.SHUT_RDWR)
                else:
                    c.send(ack)
                out, err = p.communicate(timeout=5)
                self.assertEqual(p.returncode, 1)
                self.assertEqual(out, b'')
                self.assertIn(b'ACK missing, changed, oversized or ancillary', err)
                self.facts.append({'mode': mode, 'returncode': p.returncode, 'stderr': err.decode()})
        s = self.server(nonroot=True)
        p = self.child()
        c, _ = s.accept()
        self.peers.append(c)
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 1)
        self.assertIn(b'peer is not root seqpacket', err)

    def test_PG17_S_ROOTWIRE_004_wrong_tracer_tgid_and_tracer_death(self):
        # 错误tracer是真实uid0、但不是原socket peer的TGID，不能借“都是root”放行。
        s = self.server()
        p = self.child()
        c, _ = s.accept()
        self.peers.append(c)
        request = c.recv(233)
        self.assertEqual(len(request), 232)
        read_fd, write_fd = os.pipe()
        tracer = self.owned_fork()
        if tracer == 0:
            os.close(read_fd)
            try:
                ptrace(SEIZE, p.pid, 0, EXITKILL)
                os.write(write_fd, b'R')
                waited, status = os.waitpid(p.pid, 0x40000000)
                if waited != p.pid or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 1:
                    os._exit(3)
                os._exit(0)
            except BaseException:
                os._exit(2)
        os.close(write_fd)
        try:
            self.assertTrue(select.select([read_fd], [], [], 5)[0])
            self.assertEqual(os.read(read_fd, 1), b'R')
            ack = bytearray(request[:112]);ack[12:16] = struct.pack('>I', 2);c.send(ack)
            out, err = p.communicate(timeout=5)
            self.assertEqual(self.reap_helper(tracer), 0)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(out, b'')
            self.assertIn(b'tracer TGID differs from original Root peer', err)
            self.facts.append({'wrongTracerTGIDRejected': True, 'peerTGID': os.getpid(), 'wrongTracerTGID': tracer, 'stderr': err.decode()})
        finally:
            os.close(read_fd)

        # 新root进程自行创建listener，故SO_PEERCRED TGID和真正tracer均是它；
        # 到最终SIGSTOP后直接死亡，只有EXITKILL而无detach，Native必须被内核杀死。
        Path(SOCKET).unlink(missing_ok=True)
        read_fd, write_fd = os.pipe()
        root = self.owned_fork()
        if root == 0:
            os.close(read_fd)
            try:
                listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                listener.bind(SOCKET);os.chmod(SOCKET, 0o666);listener.listen(1);listener.settimeout(5)
                os.write(write_fd, b'R')
                peer, _ = listener.accept();request = peer.recv(233)
                target, _, _ = struct.unpack('3i', peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if len(request) != 232:
                    os._exit(4)
                ptrace(SEIZE, target, 0, EXITKILL)
                ack = bytearray(request[:112]);ack[12:16] = struct.pack('>I', 2);peer.send(ack)
                wait_stop(target, signal.SIGSTOP)
                os.write(write_fd, b'S')
                os._exit(0)
            except BaseException:
                os._exit(2)
        os.close(write_fd)
        try:
            self.assertTrue(select.select([read_fd], [], [], 5)[0])
            self.assertEqual(os.read(read_fd, 1), b'R')
            p = self.child()
            self.assertTrue(select.select([read_fd], [], [], 5)[0])
            self.assertEqual(os.read(read_fd, 1), b'S')
            self.assertEqual(self.reap_helper(root), 0)
            out, err = p.communicate(timeout=5)
            self.assertEqual(p.returncode, -signal.SIGKILL)
            self.assertEqual(out, b'')
            self.facts.append({'actualRootExitedAtFinalStop': True, 'kernelEXITKILL': True, 'returncode': p.returncode})
        finally:
            os.close(read_fd)

    def test_PG17_S_ROOTWIRE_005_actual_ack_timeout(self):
        s = self.server()
        started = time.monotonic()
        p = self.child()
        c, _ = s.accept();self.peers.append(c)
        self.assertEqual(len(c.recv(233)), 232)
        out, err = p.communicate(timeout=35)
        elapsed = time.monotonic()-started
        self.assertEqual(p.returncode, 1)
        self.assertEqual(out, b'')
        self.assertGreaterEqual(elapsed, 29)
        self.assertLess(elapsed, 35)
        self.assertIn(b'ACK missing, changed, oversized or ancillary', err)
        self.facts.append({'actualNoACKTimeoutSeconds': elapsed, 'returncode': p.returncode, 'stderr': err.decode()})

    def test_PG17_S_ROOTWIRE_006_actual_buffer_M_N0_mismatch(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith(PREFIX) and not k.startswith('LD_')}
        for option, message in [('max_shared_buffers=32MB', b'maximum differs'),
                                ('shared_buffers=4MB', b'root birth M/N0 or final audit gate differs')]:
            with self.subTest(option=option):
                with tempfile.TemporaryDirectory(prefix='root-native-guc-', dir='/tmp') as temp:
                    root = Path(temp);os.chown(root, 1000, 1000)
                    data = root/'data'
                    init = subprocess.run([str(BIN.parent/'initdb'), '-D', str(data), '-A', 'trust', '--no-locale', '--no-sync'],
                                          env=env, preexec_fn=unprivileged, capture_output=True, timeout=15)
                    self.assertEqual(init.returncode, 0, init.stderr)
                    (data/'postgresql.conf').write_text("listen_addresses=''\nport=55439\nunix_socket_directories='"+temp+"'\n"
                        "enable_dynamic_shared_buffers=on\nshared_buffers='2MB'\nmax_shared_buffers='64MB'\n"
                        "huge_pages=off\nmax_connections=10\n")
                    rc, out, err = self.exchange(args=('-D', str(data), '-c', option))
                    self.assertEqual(rc, 1, err)
                    self.assertIn(message, err)


if __name__ == '__main__':
    rows = []
    class Result(unittest.TextTestResult):
        def startTest(self, test):
            self.started = time.monotonic()
            super().startTest(test)
        def addSuccess(self, test):
            super().addSuccess(test)
            rows.append({'test': test.id(), 'result': 'PASS', 'seconds': time.monotonic()-self.started})
        def addFailure(self, test, err):
            super().addFailure(test, err)
            rows.append({'test': test.id(), 'result': 'FAIL', 'detail': self._exc_info_to_string(err, test)})
        def addError(self, test, err):
            super().addError(test, err)
            rows.append({'test': test.id(), 'result': 'ERROR', 'detail': self._exc_info_to_string(err, test)})
        def addSubTest(self, test, subtest, err):
            super().addSubTest(test, subtest, err)
            if err is not None:
                rows.append({'test': test.id(), 'result': 'SUBTEST_FAILURE', 'detail': self._exc_info_to_string(err, test)})
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RootBirth)
    result = unittest.TextTestRunner(verbosity=2, resultclass=Result).run(suite)
    passed = result.wasSuccessful() and result.testsRun == 9 and len(rows) == 9
    receipt = {'passed': passed, 'expectedRoots': 9, 'testsRun': result.testsRun, 'tests': rows,
               'kernel': os.uname().release, 'sourceSHA256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'postgresSHA256': hashlib.sha256(BIN.read_bytes()).hexdigest(),
               'scope': 'ISOLATED_COMPONENT_ROOT_FIXTURE_NOT_PRODUCTION_ROOT_WAL_OR_NORMAL',
               'notProven': ['production Root/source/WAL', 'full FS/I envelope', 'safe normal decrease', 'Guest KVM', 'Kubernetes E2E']}
    (EVIDENCE/'test-result.json').write_text(json.dumps(receipt, indent=2)+'\n')
    raise SystemExit(0 if passed else 1)
