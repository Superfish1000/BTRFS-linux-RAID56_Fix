#!/bin/bash
# Does a mount read its tree roots with the write-intent record in hand?
#
#   early_record.sh <kernel> [fail-device] [omit-device]
#
# RAID6, five devices, raid6 metadata.  One device fails writes for a while
# (stale_parity in init-final3.sh), so columns of it -- data, P or Q, by
# rotation -- are left behind while the rest of their stripes move on, and the
# write-intent record names each one.  Then a DIFFERENT device is omitted.
#
# Every read of a stripe that has both is one erasure (the missing device)
# plus one wrong column.  With the wrong column unnamed that is beyond RAID6:
# two parities locate one error only if nothing else is missing.  With the
# record it is two erasures, which RAID6 rebuilds.  The record is on the disks
# the whole time -- but it used to be read only after the tree roots, and
# consulted only once a read-write recovery took it over, so the mount read
# its extent root blind and failed, and a read-only mount never consulted it at
# all.
#
# Arms, each a fresh prep then a degraded mount with $OMIT missing:
#   ro       -o ro,degraded: only the early consult can help
#   rw       -o rw,degraded: the tree roots are read before the recovery runs
#   ro-ctl   ro, btrfs.raid56_no_early_record=1 (the old behaviour)
#   rw-ctl   rw, the same
# The triggered repair is kept out of the prep so the stale columns survive to
# be read.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: early_record.sh <kernel> [fail-device] [omit-device]}
FAIL=${2:-1}
OMIT=${3:-0}
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-early.sh
ulimit -c 0
[ "$FAIL" = "$OMIT" ] && { echo "fail and omit devices must differ"; exit 2; }

arm() {	# name opts knob
	local tag=early-$1 opts=$2 knob="${3:-}" d
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/results.$tag $T/umltest/*.md5.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode omit opts extra
		local mode=$1 omit=$2 o=$3 extra="${4:-}" ubds=""
		for d in $(seq 0 $((NDEV-1))); do
			[ "$d" = "$omit" ] && continue
			ubds="$ubds ubd$d=$D/disk$d.img"
		done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-early.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=$o PROFILE=raid6:raid6 CRASH=0 \
			TAG=$tag MNTDEV=/dev/ubda NDEV=$NDEV FAIL=$FAIL \
			OMITTED=$([ "$omit" = none ] || echo "$omit") $extra \
			> $D/log.$mode 2>&1
		echo "boot $mode rc=$?" >> $T/umltest/results.$tag
	}
	boot stale_parity none rw "btrfs.raid56_no_repair_on_fault=1"
	boot stale_parity_verify $OMIT $opts "$knob"
}

verdict() {	# name -> CORRECT | REFUSED | SILENT | MOUNT_FAIL | ?
	local L=$T/umltest/early-$1/log.stale_parity_verify
	grep -q "MOUNT_FAIL" $L 2>/dev/null && { echo MOUNT_FAIL; return; }
	grep -q "STALE_SECTOR_READ_CORRECT" $L 2>/dev/null && { echo CORRECT; return; }
	grep -q "STALE_SECTOR_READ_REFUSED" $L 2>/dev/null && { echo REFUSED; return; }
	grep -q "STALE_SECTOR_SILENT_CORRUPTION" $L 2>/dev/null && { echo SILENT; return; }
	echo "?"
}
splats() { grep -l KERNEL_SPLAT $T/umltest/early-$1/log.* 2>/dev/null | wc -l; }

arm ro ro
arm rw rw
arm ro-ctl ro btrfs.raid56_no_early_record=1
arm rw-ctl rw btrfs.raid56_no_early_record=1

R=$(verdict ro); W=$(verdict rw); RC=$(verdict ro-ctl); WC=$(verdict rw-ctl)
S=$(( $(splats ro) + $(splats rw) + $(splats ro-ctl) + $(splats rw-ctl) ))
echo "  ro,degraded: $R   (control: $RC)"
echo "  rw,degraded: $W   (control: $WC)"
echo "  splats: $S"
grep -h "open_ctree failed\|failed to load\|Q syndrome" $T/umltest/early-*-ctl/log.stale_parity_verify 2>/dev/null |
	sed 's/^/    control: /' | head -3
echo
case "$R$W$RC$WC" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
[ "$S" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$RC" = CORRECT ] && [ "$WC" = CORRECT ]; then
	echo "RESULT: INCONCLUSIVE -- the old behaviour read everything correctly"
	echo "        too, so this state did not need the record"
	exit 2
fi
[ "$R" = CORRECT ] && [ "$W" = CORRECT ] || {
	echo "RESULT: FAIL -- with the record consulted from the first read: ro $R, rw $W"
	exit 1
}
echo "RESULT: PASS -- with the record consulted from the first read both mounts"
echo "        come up and read the file correctly; without it: ro $RC, rw $WC"
