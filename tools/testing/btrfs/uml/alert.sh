#!/bin/bash
# Does a person find out when RAID5/6 writes get into trouble?
#
#   alert.sh <kernel>
#
# See "alert" in init-final3.sh.  Two boots, RAID5 over four devices:
#   fixed    one episode: a fault the parity covers (degraded), healed and
#            repaired (ok), a device failing for good -- a write refused with
#            EIO, the repair given up (failing) -- then healed and scrubbed (ok).
#            Every channel must say so: the kernel log, raid56_health and its
#            poll(), the uevents, fanotify FAN_FS_ERROR, and the EIO itself.
#   control  the same writes with no fault: every listener stays silent and
#            the state stays ok -- nothing cries wolf.
# The kernel needs CONFIG_FANOTIFY for the fanotify checks.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: alert.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-alert.sh.$$ && mv -f $T/umltest/init-alert.sh.$$ $T/umltest/init-alert.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $HERE/raid56_alert_listen.py $T/umltest/
ulimit -c 0

boot() {	# arm control
	local tag=alert-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-alert.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=alert OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 > $D/log 2>&1
	rm -f $D/disk*.img
}
boot fixed 0
boot control 1
# The read that cannot be verified: fixed arm pulls a second device of the row.
boot_read() {	# arm control [read_trust]
	local tag=alert-read-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/alert_read.result.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-alert.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=alert_read OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 READ_TRUST=${3:-0} > $D/log 2>&1
	rm -f $D/disk*.img
}
boot_read fixed 0
boot_read trusting 0 1
boot_read control 1

fails=0
check() {	# description condition...
	local what=$1; shift
	if "$@"; then echo "  PASS  $what"; else echo "  FAIL  $what"; fails=$((fails+1)); fi
}
is() { [ "$(cat $1 2>/dev/null)" = "$2" ]; }
has() { grep -aq -- "$2" $1 2>/dev/null; }
count() { grep -ac -- "$2" $1 2>/dev/null; }

F=$T/umltest/alert.alert-fixed; FL=$T/umltest/alert-fixed/log
C=$T/umltest/alert.alert-control; CL=$T/umltest/alert-control/log
DEVID=$(grep -ao 'ALERT devid_b=[0-9]*' $FL | sed 's/.*=//')
grep -ah "HEALTH\[\|write .*: \|later fsync\|ALERT \|scrub rc" $FL | sed 's/^/  [fixed] /'
grep -ah "ALERT " $CL | sed 's/^/  [control] /'
echo "  uevents seen:"; sed 's/^/    /' $F/uevent 2>/dev/null
echo "  fanotify seen:"; sed 's/^/    /' $F/fan 2>/dev/null

if [ -z "$DEVID" ] || ! [ -f $F/state.recovered ] || ! [ -f $C/state.recovered ]; then
	echo "RESULT: INCONCLUSIVE -- a boot did not finish"; exit 2
fi
grep -lq KERNEL_SPLAT $FL $CL && { echo "RESULT: FAIL -- kernel splat"; exit 1; }

echo "fixed:"
check "starts ok"                                is $F/state.start ok
check "a covered fault makes it degraded"        is $F/state.fault degraded
check "healed and repaired, back to ok"          is $F/state.healed ok
check "failing once a write is refused"          is $F/state.failing failing
check "ok again after heal + scrub"              is $F/state.recovered ok
check "the covered write was acknowledged"       is $F/write.b1 ok
check "the refused O_DIRECT write got EIO"       is $F/write.c_direct EIO
check "a later fsync reports the buffered refusal" is $F/write.d_fsync EIO
check "health names devid $DEVID and says replace it" has $FL "HEALTH\[failing\].*action replace-devid-$DEVID then scrub"
check "kernel log explains the covered fault"    has $F/dmesg "failed a write into full stripe"
check "kernel log explains the refusal"          has $F/dmesg "REFUSED a write into full stripe"
check "kernel log explains the give-up"          has $F/dmesg "gave up repairing full stripe"
check "kernel log says when it is healthy again" has $F/dmesg "every recorded stripe has been repaired"
check "the covered fault alone is not failing"   awk '/HEALTH=ok/ {exit} /HEALTH=failing/ {f = 1} END {exit f}' $F/uevent
check "uevent: degraded"                         has $F/uevent "BTRFS_RAID56_HEALTH=degraded"
check "uevent: failing, naming the event and devid $DEVID" has $F/uevent "BTRFS_RAID56_HEALTH=failing.*BTRFS_RAID56_DEVID=$DEVID"
check "uevent: ok again"                         has $F/uevent "BTRFS_RAID56_HEALTH=ok"
check "poll() woke at least 4 times"             test "$(count $F/poll woken)" -ge 4
check "fanotify: the O_DIRECT refusal (iomap)"   test "$(count $F/fan 'error 5 ')" -ge 1
check "fanotify: the buffered refusal too"       test "$(count $F/fan 'error 5 ')" -ge 2
echo "control:"
check "stays ok throughout" sh -c "cat $C/state.* | sort -u | grep -qx ok && [ \$(cat $C/state.* | sort -u | wc -l) -eq 1 ]"
check "every write succeeded" sh -c "! grep -l EIO $C/write.* >/dev/null 2>&1"
check "no uevent"                                test "$(count $C/uevent BTRFS_RAID56)" -eq 0
check "no poll() wake-up"                        test "$(count $C/poll woken)" -eq 0
check "no fanotify event"                        test "$(count $C/fan FAN_FS_ERROR)" -eq 0
check "no raid56 alert in the kernel log"        sh -c "! grep -aq 'REFUSED a write\|failed a write into\|health degraded\|health failing' $C/dmesg"

echo "unverifiable read:"
grep -ah "read back\|ALERT_READ" $T/umltest/alert-read-*/log | sed 's/^/  /'
read -r RU RS < $T/umltest/alert_read.result.alert-read-fixed 2>/dev/null
read -r CU CS < $T/umltest/alert_read.result.alert-read-control 2>/dev/null
got() { grep -ao 'read back \[[^]]*\]' $T/umltest/alert-read-$1/log | tail -1; }
check "the rebuild that needed a stale column is refused (EIO)" test "$(got fixed)" = "read back []"
check "counted"                                  test "${RU:-0}" -ge 1
check "state failing"                            test "${RS:-}" = failing
check "kernel log explains it"                   has $T/umltest/alert_read.alert-read-fixed "REFUSED a read of full stripe"
check "old behaviour (control): the same read comes back WRONG, as B" sh -c "echo '$(got trusting)' | grep -q ' B B B'"
check "no second hole (control): reads A, not counted, not failing" sh -c "echo '$(got control)' | grep -q ' A A A' && [ ${CU:-1} -eq 0 ] && [ '${CS:-failing}' != failing ]"

[ $fails -eq 0 ] || { echo "RESULT: FAIL -- $fails check(s) failed"; exit 1; }
echo "RESULT: PASS -- every channel reported the episode and nothing reported the control"
