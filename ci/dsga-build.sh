#!/usr/bin/env bash
# 原生内核发布门禁：每个临时集群只使用本次准确提交构建的程序。
# 本门禁不生成 VM 物理回收资格，也不访问已有数据库或部署环境。
set -euo pipefail
major="${1:?PG major required}"
case "$major" in 17|18) ;; *) echo "unsupported DSGA PostgreSQL major" >&2; exit 2 ;; esac
root=$(git rev-parse --show-toplevel)
cd "$root"
prefix="$root/build/dsga-install"
mkdir -p build/dsga-pg build/evidence
configure=(--prefix="$prefix" --enable-debug --enable-cassert --enable-injection-points
           --enable-tap-tests --without-icu --without-readline --with-openssl)
if [[ "$major" == 18 ]]; then configure+=(--with-liburing); fi
(
  cd build/dsga-pg
  ../../configure "${configure[@]}"
  make -j4
  make -j4 install
  make -j4 check
) 2>&1 | tee build/evidence/native-build-check.log
export NEON_TEST_PG_BIN="$prefix/bin"
if [[ "$major" == 17 ]]; then
  # PG17 自带 SQL 与 isolation 用例实际覆盖映射、pin、grow、fence 和 shrink。
  make -C build/dsga-pg/src/test/modules/test_dsg_region -j4 check \
    2>&1 | tee build/evidence/dsga-native.log
else
  for module in test_dsg_region18 injection_points test_aio; do
    make -C "build/dsga-pg/src/test/modules/$module" -j4 install
  done
  make -C build/dsga-pg/contrib/pg_buffercache -j4 install
  export NEON_TEST_DSG_LIBRARY="$("$prefix/bin/pg_config" --pkglibdir)/test_dsg_region18"
  test -f "${NEON_TEST_DSG_LIBRARY}.so"
  export NEON_PG18_EVIDENCE="$root/build/evidence/native-pg18"
  mkdir -p "$NEON_PG18_EVIDENCE"
  # 不用 discover 的空结果或 skip 替代真实内核验收；这里选择无需 KVM/Root
  # 资格的原生 resize 与配置矩阵，包含真实 worker AIO、pin、回放和错误边界。
  python3 - <<'NATIVE' 2>&1 | tee build/evidence/dsga-native.log
import unittest
suite = unittest.TestSuite()
loader = unittest.defaultTestLoader
for pattern, minimum in [('test_resize18.py', 10), ('test_configuration18.py', 4)]:
    found = loader.discover('src/test/modules/test_dsg_region18/t', pattern=pattern)
    if found.countTestCases() < minimum:
        raise SystemExit(f'{pattern}: required native tests missing')
    suite.addTests(found)
result = unittest.TextTestRunner(verbosity=2).run(suite)
if not result.wasSuccessful() or result.skipped:
    raise SystemExit('native DSGA gate failed or skipped required tests')
NATIVE
fi
"$prefix/bin/postgres" --version | tee build/evidence/postgres-version.txt
"$prefix/bin/pg_config" --configure > build/evidence/configure.txt
sha256sum "$prefix/bin/postgres" > build/evidence/postgres.sha256
PG_MAJOR="$major" python3 - <<'IDENTITY'
import json,os,subprocess
from pathlib import Path
p=Path('build/evidence/source-manifest.json')
p.write_text(json.dumps({
    'schema_version':1,'kind':'postgres-native-validation',
    'source_repository':'william-lbn/postgres',
    'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    'pg_major':int(os.environ['PG_MAJOR']),
    'scope':'native-kernel-not-vm','normal_qualification':False,
    'production_ready':False,
    'run_id':os.environ.get('GITHUB_RUN_ID'),
    'run_attempt':os.environ.get('GITHUB_RUN_ATTEMPT'),
    'execution_repository':os.environ.get('GITHUB_REPOSITORY'),
},indent=2)+'\n')
IDENTITY
