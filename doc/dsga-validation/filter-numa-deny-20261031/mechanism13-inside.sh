set -Eeuo pipefail
export NEON_TEST_PG_BIN=/home/nonroot/pg_install/v17/bin NEON_TEST_DSG_LIBRARY=$(/home/nonroot/pg_install/v17/bin/pg_config --pkglibdir)/test_dsg_region.so
export PYTHONDONTWRITEBYTECODE=1 NEON_PG17_EVIDENCE=/probe
/usr/bin/gcc-12 --version >/probe/compiler.txt
/usr/bin/gcc-12 -shared -fPIC -O2 -Wall -Werror -I/home/nonroot/build/v17/src/include -I/home/nonroot/vendor/postgres-v17/src/include /probe/probe.c -o /probe/migration_filter_probe.so
sha256sum /home/nonroot/pg_install/v17/bin/postgres /probe/migration_filter_probe.so /home/nonroot/vendor/postgres-v17/src/backend/storage/buffer/dsg_envelope_filter.c >/probe/image-source-and-binaries.sha256
python3 /probe/runner.py
