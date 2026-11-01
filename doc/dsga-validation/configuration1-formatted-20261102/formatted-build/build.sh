set -Eeuo pipefail
trap 'rc=$?; for metric in cpu.stat memory.peak io.stat memory.events; do cat /sys/fs/cgroup/$metric > /evidence/$metric; done; exit "$rc"' EXIT
test "$(cat /sys/fs/cgroup/memory.max)" = 8589934592
test "$(cat /sys/fs/cgroup/cpu.max)" = "600000 100000"
cat /sys/fs/cgroup/memory.max > /evidence/memory.max
cat /sys/fs/cgroup/cpu.max > /evidence/cpu.max
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=1G CCACHE_BASEDIR=/src CCACHE_COMPILERCHECK=content
cd /src
sha256sum -c /report/expected-owned4-sha256.txt > /evidence/owned4-verify.txt
cd /build
mold -run make -C src/backend/storage/buffer dsg_buffer_arrays.o -j6
mold -run make -C src/backend postgres -j6
install -m755 /build/src/backend/postgres /opt/pg18/bin/postgres
sha256sum /opt/pg18/bin/postgres /opt/pg18/lib/postgresql/test_dsg_region18.so > /evidence/artifact-sha256.txt
