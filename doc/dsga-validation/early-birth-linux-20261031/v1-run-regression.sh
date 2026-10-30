#!/usr/bin/env bash
set -Eeuo pipefail
REPORT=/data/neon-build/validation/pg17-native-birth-20261030T160032Z
set +e
timeout 240 docker run --rm --init --network none --cpus=3 --memory=3g --pids-limit=256 --cap-drop ALL \
 -e PYTHONDONTWRITEBYTECODE=1 -e USER=nonroot -e LOGNAME=nonroot -e NEON_BIRTH_EVIDENCE=/evidence \
 --mount type=bind,src=/data/neon-build/src/neon,dst=/workspace/neon,readonly \
 --mount type=bind,src="$REPORT",dst=/evidence \
 127.0.0.1:5001/neon/pg17-native-birth:79367196-v1 python3 /evidence/runner-regression.py 2>&1 | tee "$REPORT/regression.log"
result=${PIPESTATUS[0]}
set -e
printf '%s\n' "$result" > "$REPORT/regression-exit-code.txt"
exit "$result"
