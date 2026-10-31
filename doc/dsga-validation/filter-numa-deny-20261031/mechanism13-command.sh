set -Eeuo pipefail
REPORT=/data/neon-build/validation/pg17-filter-numa-deny-20261031T014858Z-722edb/mechanism13
image=127.0.0.1:5001/neon/pg17-native-filter@sha256:7e602e1cd6f958a78caae91df93545919cb6de124d65db035550ef06d8cdf30b
set +e
docker run --rm --init --network none --read-only --cap-drop ALL --security-opt seccomp=unconfined --security-opt no-new-privileges --user 1000:1000 --cpus 2 --memory 1g --memory-swap 1g --pids-limit 128 --tmpfs /tmp:rw,nosuid,nodev,size=512m -v "$REPORT:/probe:rw" --entrypoint /bin/bash "$image" /probe/inside.sh >"$REPORT/test.log" 2>&1
rc=$?
set -e
printf '%s
' "$rc" >"$REPORT/exit-code.txt"
exit "$rc"
