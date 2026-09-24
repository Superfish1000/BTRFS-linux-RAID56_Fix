#!/bin/bash
# Does the mount-time recovery repair a stripe a user scrub would repair?
#
#   recover_scrub.sh <kernel> [ndev] [fail-device]
#
# A write that a device did not take leaves an error record naming the column
# it left stale.  A user scrub reads that record and rebuilds the column from
# the parity.  The recovery that runs at every read-write mount used to do
# less: for any stripe with an error record it only verified, wrote nothing,
# and kept the record -- so the stripe stayed without redundancy until someone
# happened to run a scrub, even though the same record in the same kernel was
# enough to repair it.
#
# Boots per arm:
#   prep     a device fails writes under nodatacow overwrites; unmount cleanly
#   recover  a plain read-write mount (the recovery runs), count the records
#            left, unmount
#   parity   ro,degraded with the failing device omitted: every one of those
#            blocks is rebuilt from the parity, which says whether the
#            acknowledged value survived at all
#
# Three arms:
#   platter  ro mount, but each acknowledged block is read straight off the
#            device btrfs-map-logical names (nocow_platter.py).  Not through
#            the filesystem: its read path consults the record and
#            reconstructs a recorded block, which is right for a reader and
#            useless for asking whether the recovery rewrote the column.
#
#   fixed      recovery repairs: no record left, platters clean, parity clean
#   control    btrfs.raid56_recover_legacy=1, the old behaviour: records kept
#              and the platters still stale.  If it is clean the test does
#              not discriminate and proves nothing.
#   ambiguous  every parity recorded unusable, so the record cannot decide the
#              stripe: recovery must decline -- keep the record and leave the
#              parity that still holds the acknowledged value alone
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: recover_scrub.sh <kernel> [ndev] [fail-device]}
NDEV=${2:-4}
FAIL=${3:-1}
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-final3.sh
cp $HERE/nocow_platter.py $T/umltest/nocow_platter.py
ulimit -c 0

MNTPROBE=/dev/ubda; [ "$FAIL" = "0" ] && MNTPROBE=/dev/ubdb

arm() {	# name legacy fakebadpar
	local tag=recover-scrub-$1 legacy=$2 fakebadpar=$3 d
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/nocow.bad.*.$tag \
		$T/umltest/nocow.sticky.recovery.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done

	boot() {	# mode omit mntdev [extra]
		local mode=$1 omit=$2 mntdev=$3 extra="${4:-}" ubds=""
		for d in $(seq 0 $((NDEV-1))); do
			case " $omit " in *" $d "*) continue;; esac
			ubds="$ubds ubd$d=$D/disk$d.img"
		done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-final3.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw PROFILE=$PROFILE TAG=$tag \
			MNTDEV=$mntdev NDEV=$NDEV FAIL=$FAIL $extra \
			> $D/log.$mode.${extra// /_} 2>&1
		echo "boot $mode omit=$omit rc=$?" >> $T/umltest/results.$tag
	}

	boot nocow_persist_prep none /dev/mapper/d0 "NOPERSIST=0 FAKEBADPAR=$fakebadpar"
	boot nocow_recover none /dev/ubda "RECOVER_LEGACY=$legacy"
	boot nocow_platter none /dev/ubda
	boot nocow_probe "$FAIL" $MNTPROBE PROBE=parity
}

get() { cat $T/umltest/$1 2>/dev/null || echo "?"; }
report() {	# name
	local tag=recover-scrub-$1
	STICKY=$(get nocow.sticky.recovery.$tag)
	DISK=$(get nocow.bad.disk.$tag)
	PAR=$(get nocow.bad.parity.$tag)
	ACKED=$(grep -ho 'in-place overwrites: [0-9]* of [0-9]*' $T/umltest/$tag/log.nocow_persist_prep.* 2>/dev/null | tail -1)
	SPLAT=$(grep -l KERNEL_SPLAT $T/umltest/$tag/log.* 2>/dev/null | wc -l)
	echo "  $1: records after recovery=$STICKY  stale on platters=$DISK  lost from parity=$PAR  ($ACKED; splats=$SPLAT)"
	grep -hE 'recovery done|could not be resolved|left untouched|declin' \
		$T/umltest/$tag/log.nocow_recover.* 2>/dev/null | sed 's/^/      /' | head -4
}

arm fixed 0 0
arm control 1 0
arm ambiguous 0 1

echo "== results =="
report fixed;     F_ST=$STICKY F_DISK=$DISK F_PAR=$PAR F_SPLAT=$SPLAT
report control;   C_ST=$STICKY C_DISK=$DISK C_PAR=$PAR C_SPLAT=$SPLAT
report ambiguous; A_ST=$STICKY A_DISK=$DISK A_PAR=$PAR A_SPLAT=$SPLAT
echo

case "$F_ST$F_DISK$F_PAR$C_ST$C_DISK$A_ST$A_PAR" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
if [ $((F_SPLAT + C_SPLAT + A_SPLAT)) -ne 0 ]; then
	echo "RESULT: FAIL -- kernel splat"; exit 1
fi
if [ "$C_ST" -eq 0 ] || [ "$C_DISK" -eq 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control left nothing stale or unrecorded,"
	echo "        so a clean fixed arm proves nothing"
	exit 2
fi
if [ "$F_ST" -ne 0 ]; then
	echo "RESULT: FAIL -- the recovery kept $F_ST record(s) it could have repaired"
	exit 1
fi
if [ "$F_DISK" -ne 0 ] || [ "$F_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- the recovery retired the record but $F_DISK block(s) are"
	echo "        still stale on disk and $F_PAR lost from the parity"
	exit 1
fi
if [ "$A_ST" -eq 0 ]; then
	echo "RESULT: FAIL -- the recovery retired a record it could not decide"
	exit 1
fi
if [ "$A_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- declining still lost $A_PAR block(s) from the parity"
	exit 1
fi
echo "RESULT: PASS -- recovery repaired what the record proves (control left"
echo "        $C_DISK stale, $C_ST recorded), declined what it cannot decide"
echo "        ($A_ST kept, nothing lost)"
