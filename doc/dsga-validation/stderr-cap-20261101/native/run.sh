set -u
export CCACHE_DISABLE=1
set +e
{
 sha256sum /usr/local/pgsql/bin/postgres /postgres/src/backend/storage/buffer/dsg_envelope_filter.c
 /usr/local/pgsql/bin/pg_config --version
 /usr/local/pgsql/bin/pg_config --configure
 /usr/bin/cc --version
} >/evidence/original-program-and-toolchain.txt 2>&1
find /usr/local/pgsql/include/server -type f -print0 | sort -z | xargs -0 sha256sum >/evidence/actual-pg17-header-sha256.txt
/usr/bin/cc -fPIC -shared -O2 -g -I/usr/local/pgsql/include/server -I/usr/local/pgsql/include/internal /probe/pipe_probe17.c -o /probe/pipe_probe17.so >/evidence/compile.log 2>&1
rc=$?
echo "$rc" >/evidence/compile.exitcode
if [ "$rc" -eq 0 ]; then
 sha256sum /probe/pipe_probe17.so >/evidence/test-DSO.sha256
 export PYTHONHOME=/test-tools/usr PYTHONDONTWRITEBYTECODE=1 NEON_PG18_EVIDENCE=/evidence
 /test-tools/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 --library-path /test-tools/lib/x86_64-linux-gnu:/test-tools/usr/lib/x86_64-linux-gnu:/test-tools/usr/local/lib /test-tools/usr/bin/python3.11 /probe/driver.py
 rc=$?
fi
echo "$rc" >/evidence/test.exitcode
for metric in cpu.stat memory.peak memory.current io.stat; do
 if [ -r "/sys/fs/cgroup/$metric" ]; then cat "/sys/fs/cgroup/$metric" >"/evidence/$metric"; fi
done
exit "$rc"