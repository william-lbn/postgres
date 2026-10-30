#!/usr/bin/env bash
set -Eeuo pipefail
REPORT=/data/neon-build/validation/pg17-native-birth-acl-v2-20261030T162729Z
set +e
timeout 240 docker run --rm --init --network none --cpus=3 --memory=3g --pids-limit=256 --cap-drop ALL \
 -e PYTHONDONTWRITEBYTECODE=1 -e NEON_BIRTH_EVIDENCE=/evidence \
 --mount type=bind,src=/data/neon-build/src/neon,dst=/workspace/neon,readonly \
 --mount type=bind,src="$REPORT",dst=/evidence \
 127.0.0.1:5001/neon/pg17-native-birth:79367196-v2 python3 /evidence/runner.py 2>&1 | tee "$REPORT/test.log"
result=${PIPESTATUS[0]}
set -e
printf '%s\n' "$result" > "$REPORT/test-exit-code.txt"
exit "$result"
