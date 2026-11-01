set -u
export LD_LIBRARY_PATH=/usr/local/pgsql/lib
export CCACHE_DISABLE=1
set +e
{ sha256sum /usr/local/pgsql/bin/postgres; /usr/local/pgsql/bin/pg_config --version; /usr/bin/cc --version; id; cat /proc/self/limits; } >/evidence/program-and-limits.txt 2>&1
find /usr/local/pgsql/include/server -type f -print0 | sort -z | xargs -0 sha256sum >/evidence/actual-header-sha256.txt
/usr/bin/cc -fPIC -shared -O0 -I/usr/local/pgsql/include/server -I/usr/local/pgsql/include/internal /probe/repro17.c -o /evidence/repro17.so >/evidence/compile.log 2>&1
rc=$?
echo "$rc" >/evidence/compile.exitcode
if test "$rc" -ne 0; then exit "$rc"; fi
sha256sum /evidence/repro17.so >/evidence/DSO.sha256
/usr/local/pgsql/bin/initdb -D /evidence/data -A trust --no-locale --no-sync >/evidence/initdb.log 2>&1
rc=$?
echo "$rc" >/evidence/initdb.exitcode
if test "$rc" -ne 0; then exit "$rc"; fi
cat >>/evidence/data/postgresql.conf <<'CONF'
enable_dynamic_shared_buffers=on
shared_buffers='1MB'
max_shared_buffers='16MB'
huge_pages=off
max_connections=12
max_worker_processes=0
max_wal_senders=0
autovacuum=off
min_dynamic_shared_memory=0
dynamic_shared_memory_type=posix
dynamic_shared_buffers_dsm_budget=0
dynamic_shared_buffers_require_admission=off
dynamic_shared_buffers_require_connection_gate=off
jit=off
CONF
export NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:2048
export NEON_DSGA_ADDRESS_SPACE_MAX_BYTES=2147483648
printf "SELECT 'PG17_ZERO_CAP_STANDALONE_STARTED';\n" | /usr/local/pgsql/bin/postgres --single -D /evidence/data postgres >/evidence/startup-console.log 2>&1
rc=$?
echo "$rc" >/evidence/startup.exitcode
if test "$rc" -ne 0; then exit "$rc"; fi
cat >/evidence/repro.sql <<'SQL'
SELECT 'PG17_ZERO_CAP_STANDALONE_DSM_ENTER';
CREATE FUNCTION pg17_zero_cap_standalone_dsm() RETURNS boolean AS '/evidence/repro17.so', 'pg17_zero_cap_standalone_dsm' LANGUAGE C;
SELECT pg17_zero_cap_standalone_dsm();
SQL
/usr/local/pgsql/bin/postgres --single -D /evidence/data postgres </evidence/repro.sql >/evidence/repro-console.log 2>&1
rc=$?
echo "$rc" >/evidence/repro.exitcode
for metric in cpu.stat memory.peak memory.current io.stat; do if test -r /sys/fs/cgroup/$metric; then cat /sys/fs/cgroup/$metric >/evidence/$metric; fi; done
# 外层独立判断原Wait与原PANIC；shell不会把失败改成PG通过。
exit 0
