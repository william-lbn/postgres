set -eu
export LD_LIBRARY_PATH=/usr/local/pgsql/lib
metrics() { for x in cpu.stat memory.peak io.stat; do cat /sys/fs/cgroup/$x >/evidence/$x; done; }
trap metrics EXIT
sha256sum /usr/local/pgsql/bin/postgres /probe/shim17.so >/evidence/actual-artifacts.sha256
/usr/local/pgsql/bin/initdb -D /evidence/data -A trust --no-locale --no-sync >/evidence/initdb.log 2>&1
cp /probe/base.conf /evidence/data/postgresql.conf
export NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:2048
export NEON_DSGA_ADDRESS_SPACE_MAX_BYTES=2147483648
cat >/evidence/snapshot.sql <<'SQL'
CREATE FUNCTION pg17_dsm_snapshot() RETURNS text AS '/probe/shim17.so','pg17_dsm_snapshot' LANGUAGE C;
SELECT pg17_dsm_snapshot();
SQL
/usr/local/pgsql/bin/postgres --single -D /evidence/data postgres </evidence/snapshot.sql >/evidence/standalone-snapshot.log 2>&1
