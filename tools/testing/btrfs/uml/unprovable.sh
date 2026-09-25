#!/bin/bash
# Does scrub write a guess over good data?
#
#   unprovable.sh <kernel> [ndev] [fail-device] [victim-device]
#
# scrub_verify_one_sector() clears the error bit of a sector with no checksum
# unconditionally -- "we have no other choice but to trust it".  On RAID5/6
# every mirror above the first is a RECONSTRUCTION from the parity rather than
# another copy, so a rebuild of unchecksummed data is declared good without
# anything having checked it, lands in @repaired, and is written back.
#
# That is only dangerous when the reconstruction is wrong, which needs a second
# fault in the same vertical stripe.  So the scenario builds one:
#
#   prep    device $FAIL fails writes, leaving its columns stale while the
#           parity was updated from the values that never landed.  Unmount
#           cleanly so the record has to come off the disk.
#   scrub   device $VICTIM -- a DIFFERENT, entirely healthy device holding good
#           data -- now fails READS.  Scrub reconstructs those sectors as
#           P ^ (other columns), and the other columns include $FAIL's stale
#           ones, so the result is a value nothing ever committed.  Without a
#           checksum nothing objects, and it is written over the good data.
#   probe   every device healthy, read the file off the platters.
#
# The file is 1 MiB of 'A' with some 4K blocks overwritten with 'B'.  A wrong
# reconstruction is an XOR of unrelated content, so it is neither -- a detector
# that needs no manifest and cannot be fooled by the overwrite itself.
#
# Two arms.  The control sets raid56_scrub_trusts_rebuild=1, restoring the
# behaviour from before the guard.  A clean fixed arm means nothing unless the
# control is dirty.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: unprovable.sh <kernel> [ndev] [fail-device] [victim-device]}
NDEV=${2:-4}
FAIL=${3:-1}
VICTIM=${4:-2}
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-final3.sh.$$ && mv -f $T/umltest/init-final3.sh.$$ $T/umltest/init-final3.sh	# atomic: a guest may be reading it
ulimit -c 0

[ "$FAIL" = "$VICTIM" ] && { echo "FAIL and VICTIM must differ"; exit 2; }

arm() {	# trustrebuild [csumvictim] -> echoes "<garbage>"
	local trust=$1 csum=${2:-0}
	local tag=unprovable-$1-${2:-0} d ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/unprov.garbage.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done

	boot() {	# mode [extra] [omit]
		local mode=$1 extra="${2:-}" omit="${3:-}"
		ubds=""
		for d in $(seq 0 $((NDEV-1))); do
			case " $omit " in *" $d "*) continue;; esac
			ubds="$ubds ubd$d=$D/disk$d.img"
		done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-final3.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw,noraid56_write_intent \
			PROFILE=$PROFILE TAG=$tag \
			MNTDEV=/dev/mapper/d0 NDEV=$NDEV FAIL=$FAIL VICTIM=$VICTIM $extra \
			> $D/log.$mode 2>&1
		echo "boot $mode rc=$?" >> $T/umltest/results.$tag
	}

	boot unprovable_prep "CSUMVICTIM=$csum"
	boot unprovable_diag "CSUMVICTIM=$csum" "$VICTIM"
	boot unprovable_scrub "TRUSTREBUILD=$trust CSUMVICTIM=$csum"
	boot unprovable_probe "CSUMVICTIM=$csum"
	cat $T/umltest/unprov.garbage.$tag 2>/dev/null || echo "?"
}

echo "== unchecksummed victim, guard in place =="
fixed=$(arm 0 0)
grep -hE 'UNPROV_|not written back' \
	$T/umltest/unprovable-0-0/log.* 2>/dev/null | sed 's/^/  /' | head -6

echo "== unchecksummed victim, control: raid56_scrub_trusts_rebuild=1 =="
ctrl=$(arm 1 0)
grep -hE 'UNPROV_' $T/umltest/unprovable-1-0/log.* 2>/dev/null | sed 's/^/  /' | head -4

# The victim is CHECKSUMMED here, so the guard never applies to it: a
# reconstruction that consumed a stale column does not match the stored
# checksum.  The claim under test is that it is written back regardless.  Run
# it with the guard OFF, so nothing but the checksum stands between the guess
# and the platter.
echo "== CHECKSUMMED victim, guard off (tests the claim, not the guard) =="
csumbad=$(arm 1 1)
grep -hE 'UNPROV_|unrepaired|csum mismatch' \
	$T/umltest/unprovable-1-1/log.* 2>/dev/null | sed 's/^/  /' | head -6

echo
echo "4K blocks never overwritten by the test that no longer read back as 'A',"
echo "read from healthy devices after the scrub:"
echo "  unchecksummed, guard in place : $fixed"
echo "  unchecksummed, control (off)  : $ctrl"
echo "  checksummed,   guard off      : $csumbad"
case "$csumbad" in
	0) echo "     -> the checksum rejected every wrong rebuild; none reached the disk." ;;
	*) echo "     -> WITH A CHECKSUM PRESENT, $csumbad block(s) were still destroyed." ;;
esac

for f in $T/umltest/unprovable-*/log.*; do
	grep -q KERNEL_SPLAT "$f" 2>/dev/null && { echo "RESULT: FAIL -- kernel splat in $f"; exit 1; }
done
case "$fixed:$ctrl" in
	0:0)   echo "RESULT: INCONCLUSIVE -- the control destroyed nothing either, so"
	       echo "        the scenario did not reach the path.  Try other FAIL/VICTIM"
	       echo "        devices, or more overwrites." ; exit 3 ;;
	0:*)   echo "RESULT: PASS -- the control wrote $ctrl block(s) of guesswork over good"
	       echo "        data; with the guard, none." ; exit 0 ;;
	?:*)   echo "RESULT: FAIL -- the guard did not prevent $fixed destroyed block(s)." ; exit 1 ;;
esac
