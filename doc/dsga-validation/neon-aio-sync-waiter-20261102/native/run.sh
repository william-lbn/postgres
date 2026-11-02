set -Eeuo pipefail
trap 'rc=$?; for metric in cpu.stat memory.peak io.stat memory.events cpu.max memory.max;do cat /sys/fs/cgroup/$metric >/evidence/$metric;done;exit "$rc"' EXIT
test "$(cat /sys/fs/cgroup/memory.max)" = 3221225472
test "$(cat /sys/fs/cgroup/cpu.max)" = "200000 100000"
cat /proc/sys/kernel/io_uring_disabled >/evidence/host-io_uring_disabled.txt || true
sha256sum /opt/pg18/bin/postgres /opt/pg18/lib/postgresql/neon.so /opt/pg18/lib/postgresql/neon_rmgr.so /input/pg18_neon_aio_probe.so >/evidence/actual-artifacts.sha256
export NEON_TEST_PG_BIN=/opt/pg18/bin NEON_PG18_EVIDENCE=/evidence NEON_PG18_AIO_PROBE=/input/pg18_neon_aio_probe.so LD_LIBRARY_PATH=/opt/pg18/lib
python3 -B -u /input/test_dsg_pg18_aio_waiter.py
