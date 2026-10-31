#!/bin/busybox sh
export PATH=/bin:/usr/bin:/opt/pg18/bin
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /dev/pts /run /tmp /sys/fs/cgroup /sys/kernel/security
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t devpts devpts /dev/pts
mount -t tmpfs -o size=16m,mode=755 tmpfs /run
mount -t tmpfs -o size=1g,mode=1777 tmpfs /tmp
mount -t cgroup2 none /sys/fs/cgroup
mount -t securityfs none /sys/kernel/security
export LANG=C LC_ALL=C PYTHONDONTWRITEBYTECODE=1
export NEON_TEST_PG_BIN=/opt/pg18/bin
export NEON_TEST_DSG_LIBRARY=/opt/pg18/lib/postgresql/test_dsg_region18.so
export NEON_DSGA_ROOT_COMPONENT_TEST=1
release=$(uname -r)
notes=$(sha256sum /sys/kernel/notes | cut -d' ' -f1)
echo "PG18_GUEST_RELEASE=$release"
echo "PG18_GUEST_NOTES=$notes"
if [ "$release" != "6.12.26-william-normal-split-v1-e210f5e3ee94" ] || [ "$notes" != "7cf45ced24c800ec8390fc7f04f07b8dc9e73ac0efa4bf8abd11d8513c8eb828" ]; then
 echo PG18_GUEST_RESULT=90
 sync
 poweroff -f
fi
cat /proc/cmdline > /evidence/cmdline.txt
uname -a > /evidence/uname.txt
sha256sum /sys/kernel/notes /opt/pg18/bin/postgres /opt/pg18/lib/postgresql/test_dsg_region18.so > /evidence/actual-artifact-hashes.txt
cat /proc/sys/kernel/neonvm_pid_max_limit > /evidence/pid-hard-limit.txt
cat /sys/kernel/security/lsm > /evidence/lsm.txt
cat /proc/self/status > /evidence/init-status.txt
overall=0
for phase in filter5 root9; do
 export NEON_PG18_EVIDENCE=/evidence/$phase
 export NEON_ARRAY_ROOT_EVIDENCE=/evidence/$phase
 export NEON_ROOT_WIRE_EVIDENCE=/evidence/$phase
 /usr/bin/python3 /fixture/runner.py "$phase" > "/evidence/$phase/run.log" 2>&1
 rc=$?
 echo "$rc" > "/evidence/$phase/test.rc"
 cat "/evidence/$phase/run.log"
 echo "PG18_GUEST_PHASE=$phase RC=$rc"
 if [ "$rc" -ne 0 ]; then overall=1; fi
done
echo "$overall" > /evidence/overall.rc
echo "PG18_GUEST_RESULT=$overall"
sync
poweroff -f
