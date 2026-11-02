set -Eeuo pipefail
trap 'rc=$?; for metric in cpu.stat memory.peak io.stat memory.events; do cat /sys/fs/cgroup/$metric > /evidence/$metric; done; exit "$rc"' EXIT
test "$(cat /sys/fs/cgroup/memory.max)" = 8589934592
test "$(cat /sys/fs/cgroup/cpu.max)" = "600000 100000"
cat /sys/fs/cgroup/cpu.max >/evidence/cpu.max
cat /sys/fs/cgroup/memory.max >/evidence/memory.max
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=1G CCACHE_BASEDIR=/src CCACHE_COMPILERCHECK=content
mold -run make -C /build/src/backend/storage/aio aio.o -j6
python3 - <<'INNER'
import json,subprocess
from pathlib import Path
cmd=['mold','-run',*json.loads(Path('/report/original-link-argv.json').read_text())]
subprocess.run(cmd,cwd='/build/src/backend',check=True)
INNER
install -m755 /build/src/backend/postgres /opt/pg18/bin/postgres
sha256sum /opt/pg18/bin/postgres /opt/pg18/lib/postgresql/test_dsg_region18.so >/evidence/core-artifacts.sha256
nm -D /opt/pg18/bin/postgres | grep ' pgaio_io_start_readv_synchronous$' >/evidence/new-actual-symbol.txt
/opt/pg18/bin/postgres --version >/evidence/postgres-version.txt
