#!/usr/bin/env bash
set -Eeuo pipefail
BASE=/data/neon-build
REPORT=/data/neon-build/validation/pg17-native-birth-acl-v2-20261030T162729Z
source "$BASE/config/env.sh"
git -C "$BASE/src/neon" rev-parse HEAD > "$REPORT/neon-head.txt"
git -C "$BASE/src/neon/vendor/postgres-v17" rev-parse HEAD > "$REPORT/pg-head.txt"
git -C "$BASE/src/neon/vendor/postgres-v17" status --short > "$REPORT/pg-status.txt"
sha256sum "$REPORT/Dockerfile" > "$REPORT/Dockerfile.sha256"
set +e
docker buildx build --builder neon-data --network host --allow network.host --progress plain \
 --metadata-file "$REPORT/build-metadata.json" \
 --output type=image,name=127.0.0.1:5001/neon/pg17-native-birth:79367196-v2,push=true,registry.insecure=true \
 -f "$REPORT/Dockerfile" "$BASE/src/neon" 2>&1 | tee "$REPORT/build.log"
result=${PIPESTATUS[0]}
set -e
printf '%s\n' "$result" > "$REPORT/build-exit-code.txt"
test "$result" = 0
docker pull 127.0.0.1:5001/neon/pg17-native-birth:79367196-v2 > "$REPORT/pull.log" 2>&1
