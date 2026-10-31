#!/usr/bin/env bash
set -Eeuo pipefail
REPORT=/data/neon-build/validation/pg17-filter-numa-deny-20261031T014858Z-722edb/compatibility11
set +e
timeout 240 docker run --rm --init --network none --cpus=2 --memory=2g --pids-limit=256 --cap-drop ALL -e PYTHONDONTWRITEBYTECODE=1 -e NEON_BIRTH_EVIDENCE=/evidence --mount type=bind,src="$REPORT",dst=/evidence 127.0.0.1:5001/neon/pg17-native-filter@sha256:7e602e1cd6f958a78caae91df93545919cb6de124d65db035550ef06d8cdf30b python3 /evidence/runner.py 2>&1 | tee "$REPORT/run.log"
rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$rc" > "$REPORT/exit-code.txt"
exit "$rc"
