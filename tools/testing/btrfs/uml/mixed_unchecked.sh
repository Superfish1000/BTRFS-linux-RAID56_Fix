#!/bin/bash
# Does a read of data without checksums from a MIXED (data+metadata) RAID6
# block group refuse a rebuild nothing can check?
#
#   mixed_unchecked.sh <kernel>
#
# See mixed_unchecked in init-final3.sh.  mkfs --mixed, RAID6 over five
# devices, a nodatasum (parity) or nodatacow (stale) file of 'A', then a
# degraded read-only mount whose rebuild of one block nothing can vouch for:
#   parity   Q of that row overwritten, the data column's device gone: the
#            mirror-3 retry rebuilds from Q alone
#   stale    column B's write failed (recorded), C's and P's devices gone:
#            C's rebuild folds in B's stale content
#   reloc    as parity, but read-write, and the missing device removed:
#            relocation reads the block through the same rebuilds
#   fixed    EIO, and read_parities_disagree / read_unverifiable counted; for
#            reloc the removal fails rather than copy the rebuild
#   control  raid56_read_trusts_ambiguous=1, the old answer for a mixed block
#            group: the wrong block, no error.  For reloc
#            raid56_read_trusts_mixed_reloc=1, the old answer for relocation:
#            the removal finishes and the block reads back wrong from the new
#            chunk with every device present
# SUBS="parity", SUBS="stale" or SUBS="reloc" runs one of them.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: mixed_unchecked.sh <kernel>}
NDEV=5
SUBS=${SUBS:-parity stale reloc}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-mixu.sh.$$ && mv -f $T/umltest/init-mixu.sh.$$ $T/umltest/init-mixu.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ && mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# sub name control
	local tag=mixu-$1-$2 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/mixu.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-mixu.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=mixed_unchecked SUB=$1 OPTS=rw PROFILE=raid6:raid6 TAG=$tag \
		NDEV=$NDEV CONTROL=$3 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
reloc_verdict() {	# as verdict(), for SUB=reloc
	local s=$1 a
	local f_mix f_ack f_src f_sa f_trc f_ta f_n f_ref f_rm
	local c_mix c_ack c_src c_sa c_trc c_ta c_n c_ref c_rm
	for a in fixed control; do
		grep -ah "mixed RAID6\|layout:\|overwritten\|device remove\|after it:\|MIXU \|control:\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|DM_RELOAD\|REMOVE_FAIL\|CORRUPT_FAIL" \
			$T/umltest/mixu-$s-$a/log | sed "s/^/  [$s $a] /"
	done
	read -r f_mix f_ack f_src f_sa f_trc f_ta f_n f_ref f_rm < $T/umltest/mixu.mixu-$s-fixed 2>/dev/null || f_mix=?
	read -r c_mix c_ack c_src c_sa c_trc c_ta c_n c_ref c_rm < $T/umltest/mixu.mixu-$s-control 2>/dev/null || c_mix=?
	case "$f_mix$c_mix" in *'?'*) echo "$s: INCONCLUSIVE -- a boot did not report"; return 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/mixu-$s-*/log && { echo "$s: FAIL -- kernel splat"; return 1; }
	if [ "$f_mix$c_mix" != 11 ]; then
		echo "$s: INCONCLUSIVE -- mkfs did not make a mixed RAID6 block group"; return 2
	fi
	if [ "$f_src/$f_sa $c_src/$c_sa" != "0/4096 0/4096" ]; then
		echo "$s: INCONCLUSIVE -- a block the damage does not touch did not read back ($f_src/$f_sa, $c_src/$c_sa)"
		return 2
	fi
	if [ "$c_rm" != 0 ] || [ "$c_trc" != 0 ] || [ "$c_ta" = 4096 ]; then
		echo "$s: INCONCLUSIVE -- the control's relocation did not copy a wrong block (remove rc $c_rm, read rc $c_trc, $c_ta of 4096 right)"
		return 2
	fi
	if [ "$f_trc" = 0 ] && [ "$f_ta" != 4096 ]; then
		echo "$s: FAIL -- the block reads back wrong with no error ($f_ta of 4096 bytes right, remove rc $f_rm)"; return 1
	fi
	if [ "$f_rm" = 0 ]; then
		echo "$s: FAIL -- the removal finished although the block could only be rebuilt unchecked"; return 1
	fi
	if [ "${f_n:-0}" -lt 1 ]; then
		echo "$s: FAIL -- the relocation failed but no alert counted it"; return 1
	fi
	echo "$s: PASS -- the removal failed (rc $f_rm) with $f_n alert(s); trusting the relocation's rebuild copied a wrong block ($c_ta of 4096 bytes right)"
	return 0
}
verdict() {	# sub: prints the verdict, returns 0 pass, 1 fail, 2 inconclusive
	local s=$1 a
	[ "$s" = reloc ] && { reloc_verdict $s; return; }
	local f_mix f_ack f_src f_sa f_trc f_ta f_n f_ref
	local c_mix c_ack c_src c_sa c_trc c_ta c_n c_ref
	for a in fixed control; do
		grep -ah "mixed RAID6\|layout:\|acknowledged\|refused\|overwritten\|MIXU \|control:\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|DM_RELOAD\|REMOVE_FAIL\|CORRUPT_FAIL" \
			$T/umltest/mixu-$s-$a/log | sed "s/^/  [$s $a] /"
	done
	read -r f_mix f_ack f_src f_sa f_trc f_ta f_n f_ref < $T/umltest/mixu.mixu-$s-fixed 2>/dev/null || f_mix=?
	read -r c_mix c_ack c_src c_sa c_trc c_ta c_n c_ref < $T/umltest/mixu.mixu-$s-control 2>/dev/null || c_mix=?
	case "$f_mix$c_mix" in *'?'*) echo "$s: INCONCLUSIVE -- a boot did not report"; return 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/mixu-$s-*/log && { echo "$s: FAIL -- kernel splat"; return 1; }
	if [ "$f_mix$c_mix" != 11 ]; then
		echo "$s: INCONCLUSIVE -- mkfs did not make a mixed RAID6 block group"; return 2
	fi
	if [ "$f_ack$c_ack" != 11 ]; then
		echo "$s: INCONCLUSIVE -- the write into column B was refused, nothing is stale"; return 2
	fi
	if [ "$f_src/$f_sa $c_src/$c_sa" != "0/4096 0/4096" ]; then
		echo "$s: INCONCLUSIVE -- a block the damage does not touch did not read back ($f_src/$f_sa, $c_src/$c_sa)"
		return 2
	fi
	if [ "$c_trc" != 0 ] || [ "$c_ta" = 4096 ]; then
		echo "$s: INCONCLUSIVE -- the control did not return a wrong block (rc $c_trc, $c_ta of 4096 right), so a refusal proves nothing"
		return 2
	fi
	if [ "$f_trc" = 0 ] && [ "$f_ta" != 4096 ]; then
		echo "$s: FAIL -- the read returned a wrong block with no error ($f_ta of 4096 bytes right)"; return 1
	fi
	if [ "$f_trc" = 0 ]; then
		echo "$s: INCONCLUSIVE -- the read came back right, so it never needed the rebuild the control got"
		return 2
	fi
	if [ "${f_n:-0}" -lt 1 ]; then
		echo "$s: FAIL -- the read failed but no alert counted it"; return 1
	fi
	[ "$f_ref" = 1 ] || echo "  note: $s: no 'REFUSED a read' line in the fixed arm's kernel log"
	echo "$s: PASS -- EIO with $f_n alert(s); trusting the rebuild returned a wrong block ($c_ta of 4096 bytes right)"
	return 0
}
for s in $SUBS; do
	arm $s fixed 0
	arm $s control 1
done
worst=0
for s in $SUBS; do
	verdict $s
	r=$?
	[ $r = 1 ] && worst=1
	[ $r = 2 ] && [ $worst = 0 ] && worst=2
done
case $worst in
0) echo "RESULT: PASS -- nodatasum data in a mixed RAID6 block group is refused, not rebuilt unchecked, relocation included";;
1) echo "RESULT: FAIL";;
*) echo "RESULT: INCONCLUSIVE";;
esac
exit $worst
