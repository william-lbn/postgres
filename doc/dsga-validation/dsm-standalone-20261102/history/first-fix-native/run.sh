set -eu
export LD_LIBRARY_PATH=/usr/local/pgsql/lib
export CCACHE_DISABLE=1
metrics() { for x in cpu.stat memory.peak memory.current io.stat; do if test -r /sys/fs/cgroup/$x; then cat /sys/fs/cgroup/$x >/evidence/$x; fi; done; }
trap metrics EXIT
{ sha256sum /usr/local/pgsql/bin/postgres; /usr/local/pgsql/bin/pg_config --version; /usr/local/pgsql/bin/pg_config --configure; id; } >/evidence/program-and-toolchain.txt 2>&1
find /usr/local/pgsql/include/server -type f -print0 | sort -z | xargs -0 sha256sum >/evidence/actual-header-sha256.txt
/usr/bin/cc -fPIC -shared -O0 -I/usr/local/pgsql/include/server -I/usr/local/pgsql/include/internal /probe/shim17.c -o /evidence/shim17.so >/evidence/shim-compile.log 2>&1
sha256sum /evidence/shim17.so >/evidence/shim-DSO.sha256
/usr/local/pgsql/bin/initdb -D /evidence/data -A trust --no-locale --no-sync >/evidence/initdb.log 2>&1
mkdir /evidence/socket
cat >/evidence/base.conf <<'CONF'
listen_addresses=''
port=55432
unix_socket_directories='/evidence/socket'
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
cp /evidence/base.conf /evidence/data/postgresql.conf
cat >/evidence/ddl.sql <<'SQL'
CREATE OR REPLACE FUNCTION pg17_dsm_cycle() RETURNS boolean AS '/evidence/shim17.so','pg17_dsm_cycle' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_snapshot() RETURNS text AS '/evidence/shim17.so','pg17_dsm_snapshot' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_hold(bigint) RETURNS bigint AS '/evidence/shim17.so','pg17_dsm_hold' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_release(bigint) RETURNS boolean AS '/evidence/shim17.so','pg17_dsm_release' LANGUAGE C;
CREATE OR REPLACE FUNCTION pg17_dsm_pgstat_area() RETURNS void AS '/evidence/shim17.so','pg17_dsm_pgstat_area' LANGUAGE C;
SQL
export NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:2048
export NEON_DSGA_ADDRESS_SPACE_MAX_BYTES=2147483648
printf "SELECT 'PG17_ZERO_CAP_STANDALONE_STARTED';\n" | /usr/local/pgsql/bin/postgres --single -D /evidence/data postgres >/evidence/standalone-startup.log 2>&1
cat /evidence/ddl.sql >/evidence/standalone.sql
printf "SELECT CASE WHEN pg17_dsm_cycle() THEN 'PG17_STANDALONE_ACTUAL_DSM_OK' ELSE 'FAIL' END;\n" >>/evidence/standalone.sql
/usr/local/pgsql/bin/postgres --single -D /evidence/data postgres </evidence/standalone.sql >/evidence/standalone-DSM.log 2>&1
if grep -Eq 'ERROR:|PANIC:|FATAL:' /evidence/standalone-DSM.log; then exit 21; fi
grep -q PG17_STANDALONE_ACTUAL_DSM_OK /evidence/standalone-DSM.log
printf 0 >/evidence/zero-standalone.exitcode
set +e
printf 'SELECT 1;\n' | /usr/local/pgsql/bin/postgres --single -D /evidence/data -c dynamic_shared_buffers_dsm_budget=8 postgres >/evidence/positive-standalone.log 2>&1
rc=$?
echo "$rc" >/evidence/positive-standalone.exitcode
set -e
test "$rc" -ne 0
grep -q 'requires a fork Postmaster environment' /evidence/positive-standalone.log
set +e
printf 'SELECT 1;\n' | env -u NEON_DSGA_EARLY_ARRAYS_PROFILE -u NEON_DSGA_ADDRESS_SPACE_MAX_BYTES /usr/local/pgsql/bin/postgres --single -D /evidence/data -c enable_dynamic_shared_buffers=off -c max_shared_buffers=0 -c dynamic_shared_buffers_dsm_budget=8 postgres >/evidence/positive-nativeoff.log 2>&1
rc=$?
echo "$rc" >/evidence/positive-nativeoff.exitcode
set -e
test "$rc" -ne 0
grep -q 'requires managed native buffer profile' /evidence/positive-nativeoff.log
psql_owned() { /usr/local/pgsql/bin/psql -h /evidence/socket -p 55432 -U nonroot -d postgres -XqAt -v ON_ERROR_STOP=1 "$@"; }
/usr/local/pgsql/bin/pg_ctl -D /evidence/data -l /evidence/baseline-server.log -w -t 20 start >/evidence/baseline-start.log 2>&1
psql_owned -c 'SELECT pg17_dsm_snapshot()' >/evidence/baseline.json
psql_owned -c 'SELECT pg17_dsm_pgstat_area()' >/evidence/baseline-dump-query.log
baseline=$(sed -n 's/.*"reserved_upper_bytes":\([0-9]*\).*/\1/p' /evidence/baseline.json)
case "$baseline" in ''|*[!0-9]*) exit 22;; esac
echo "$baseline" >/evidence/measured-baseline-bytes.txt
capkb=$(( (baseline + 8192 + 1023) / 1024 ))
echo "$capkb" >/evidence/fixed-positive-cap-kB.txt
/usr/local/pgsql/bin/pg_ctl -D /evidence/data -m fast -w -t 15 stop >/evidence/baseline-stop.log 2>&1
cp /evidence/base.conf /evidence/data/postgresql.conf
printf "dynamic_shared_buffers_dsm_budget='%skB'\n" "$capkb" >>/evidence/data/postgresql.conf
/usr/local/pgsql/bin/pg_ctl -D /evidence/data -l /evidence/positive-server.log -w -t 20 start >/evidence/positive-start.log 2>&1
psql_owned -c 'SELECT pg17_dsm_snapshot()' >/evidence/positive-initial.json
psql_owned -c 'SELECT pg17_dsm_pgstat_area()' >/evidence/positive-dump-query.log
handle=$(psql_owned -c 'SELECT pg17_dsm_hold(8192)')
case "$handle" in ''|*[!0-9]*) exit 23;; esac
echo "$handle" >/evidence/actual-held-handle.txt
test $((handle % 2)) -eq 0
psql_owned -c 'SELECT pg17_dsm_snapshot()' >/evidence/positive-held.json
stat -c '%n %s' /dev/shm/PostgreSQL.* | sort >/evidence/OS-before-rejected-create.txt
set +e
psql_owned -c 'SELECT pg17_dsm_hold(4096)' >/evidence/over-cap.stdout 2>/evidence/over-cap.stderr
rc=$?
echo "$rc" >/evidence/over-cap.exitcode
set -e
test "$rc" -ne 0
grep -q 'external DSM budget or registry capacity is exhausted' /evidence/over-cap.stderr
stat -c '%n %s' /dev/shm/PostgreSQL.* | sort >/evidence/OS-after-rejected-create.txt
cmp /evidence/OS-before-rejected-create.txt /evidence/OS-after-rejected-create.txt
psql_owned -c 'SELECT pg17_dsm_snapshot()' >/evidence/positive-rejected.json
psql_owned -c "SELECT pg17_dsm_release($handle)" >/evidence/release.log
psql_owned -c 'SELECT pg17_dsm_snapshot()' >/evidence/positive-released.json
/usr/local/pgsql/bin/pg_ctl -D /evidence/data -m fast -w -t 15 stop >/evidence/positive-stop.log 2>&1
printf 0 >/evidence/fork-positive.exitcode
