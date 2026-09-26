#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# After a crash while degraded, does a checksummed sector that matches no
# rebuild make a RAID6 stripe undecidable?
#
#   degraded_csum.sh <kernel>
#
# See degraded_csum_prep, degraded_crash and degraded_csum_read in
# init-final3.sh.  RAID6 data, RAID1C3 metadata, four devices.  One block of
# 'S's with a checksum, then nodatacow files of 'A's right after it: column B
# of the checksummed block's full stripe holds it and nodatacow blocks in its
# other rows, column C a nodatacow block in the same row.  B's device is left
# out, and a write into that block of C, mounted degraded, is torn by
# raid56_crash_point=1: C lands, neither parity does.  With both parities
# still describing the old C, solving B and C from them together gives the
# checksummed block back; so the host then overwrites Q's sector of that row
# with garbage, as a parity write torn within the sector would leave it.
# Then a degraded read-write mount, whose recovery compares the rebuilds of B
# from P and from Q: in the torn row they disagree, and the checksummed block
# matches no rebuild; in the other rows they agree.
#   fixed    the checksummed sector is left unrepaired (its checksum refuses
#            every read of it), and the stripe is not recorded undecidable:
#            B's nodatacow blocks read 'A', which the read's Q cross-check
#            vouches for
#   control  btrfs.raid56_recover_absent_checked_suspect=1, as the first
#            version of the classification did: the stripe is recorded
#            undecidable (every parity stale), and B's nodatacow blocks,
#            which nothing tore, read EIO
# Either way the checksummed block reads 'S' or fails, and no block of B reads
# back wrong.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: degraded_csum.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-dcs.sh.$$ &&
	mv -f $T/umltest/init-dcs.sh.$$ $T/umltest/init-dcs.sh
cp $HERE/raid56_csum_layout.py $T/umltest/raid56_csum_layout.py.$$ &&
	mv -f $T/umltest/raid56_csum_layout.py.$$ $T/umltest/raid56_csum_layout.py
ulimit -c 0
arm() {	# name [kernel args...]
	local tag=dcs-$1 idx_b
	local D=$T/umltest/$tag
	shift
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/layout.$tag $T/umltest/dcs.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode omit opts [kernel args...]
		local mode=$1 omit=$2 opts=$3 ubds="" mnt="" d
		shift 3
		for d in $(seq 0 $((NDEV-1))); do
			[ "$d" = "$omit" ] && continue
			ubds="$ubds ubd$d=$D/disk$d.img"
			[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
		done
		timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-dcs.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=$opts PROFILE=raid6:raid1c3 TAG=$tag \
			NDEV=$NDEV MNTDEV=$mnt "$@" < /dev/null > $D/log.$mode 2>&1
		echo "boot $mode omit=$omit rc=$?" >> $D/log.boots
	}
	boot degraded_csum_prep none rw
	idx_b=$(sed -n 's/.*IDX_B=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	idx_q=$(sed -n 's/.*IDX_Q=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	phys_q=$(sed -n 's/.*PHYS_Q=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	if [ -n "$idx_b" ] && [ -n "$idx_q" ] && [ -n "$phys_q" ]; then
		boot degraded_crash "$idx_b" rw CRASH=1 NAMED=0 "$@"
		head -c 4096 /dev/zero | tr '\000' '\377' |
			dd of=$D/disk$idx_q.img bs=4096 seek=$((phys_q / 4096)) count=1 \
			   conv=notrunc status=none &&
			echo "Q's sector of the torn row overwritten: disk$idx_q at $phys_q" \
				>> $D/log.boots
		boot degraded_csum_read "$idx_b" rw "$@"
	fi
	rm -f $D/disk*.img
}
arm fixed
arm control btrfs.raid56_recover_absent_checked_suspect=1
SHOW="layout:|knob |crash armed|NO_CRASH|crash injection|DCS |WRONG|match neither"
SHOW="$SHOW|parities disagree|possibly torn|CHATTR_FAIL|LAYOUT_FAIL|KERNEL_SPLAT"
SHOW="$SHOW|MOUNT_FAIL|MKFS_FAIL|overwritten"
for a in fixed control; do
	grep -ahE "$SHOW" $T/umltest/dcs-$a/log.* 2>/dev/null | sed "s/^/  [$a] /"
done
# ok eio wrong csum-block recovery_suspect torn_undecidable
res() { cat $T/umltest/dcs.dcs-$1 2>/dev/null || echo "? ? ? ? ? ?"; }
read -r f_ok f_eio f_wrong f_cs f_sus f_und <<<"$(res fixed)"
read -r c_ok c_eio c_wrong c_cs c_sus c_und <<<"$(res control)"
echo "  column B's nodatacow blocks ok/eio/wrong, checksummed block, recovery_suspect," \
     "torn_undecidable:"
echo "    fixed   $f_ok/$f_eio/$f_wrong, $f_cs, $f_sus, $f_und"
echo "    control $c_ok/$c_eio/$c_wrong, $c_cs, $c_sus, $c_und"
case "$f_ok$f_sus$c_ok$c_sus" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
if grep -alq KERNEL_SPLAT $T/umltest/dcs-*/log.* 2>/dev/null; then
	echo "RESULT: FAIL -- kernel splat"
	exit 1
fi
for a in fixed control; do
	grep -aq "crash injection 1 at full stripe" $T/umltest/dcs-$a/log.degraded_crash \
		2>/dev/null && grep -aq "overwritten" $T/umltest/dcs-$a/log.boots && continue
	echo "RESULT: INCONCLUSIVE -- $a never crashed in the torn write, or Q was not torn"
	exit 2
done
grep -aq "knob raid56_recover_absent_checked_suspect=Y" \
	$T/umltest/dcs-control/log.degraded_csum_read || {
	echo "RESULT: INCONCLUSIVE -- raid56_recover_absent_checked_suspect is not set in the"
	echo "        control (not a CONFIG_BTRFS_DEBUG kernel?)"
	exit 2
}
if [ "$f_wrong" != 0 ] || [ "$c_wrong" != 0 ] || [ "$f_cs" = wrong ] ||
   [ "$c_cs" = wrong ]; then
	echo "RESULT: FAIL -- column B read back wrong without an error: fixed $f_wrong" \
	     "(checksummed block $f_cs), control $c_wrong (checksummed block $c_cs)"
	exit 1
fi
if [ "$f_cs" != eio ] || [ "$c_sus" = 0 ] || [ "$c_eio" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the checksummed block read $f_cs, and the control recorded"
	echo "        $c_sus stripe(s) undecidable and refused $c_eio block(s): no checksummed"
	echo "        sector matching no rebuild came to the classification"
	exit 2
fi
if [ "$f_sus" != 0 ] || [ "$f_eio" != 0 ]; then
	echo "RESULT: FAIL -- a checksummed sector matching no rebuild still made the"
	echo "        stripe undecidable (recovery_suspect $f_sus), or refused reads its"
	echo "        parities agree on ($f_eio of $((f_ok + f_eio)))"
	exit 1
fi
echo "RESULT: PASS -- the checksummed sector that matched no rebuild was left"
echo "        unrepaired ($f_cs), and B's other $f_ok blocks read back correctly; recorded"
echo "        undecidable for it, the stripe refused $c_eio of them"
