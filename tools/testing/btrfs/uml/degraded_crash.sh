#!/bin/bash
# Does the recovery of a crash while degraded hand back a guess as data?
#
#   degraded_crash.sh <kernel>
#
# See degraded_crash_prep, degraded_crash and degraded_crash_read in
# init-final3.sh.  A nodatacow file of 'A's on RAID5 (three devices) and on
# RAID6 (four).  The device holding data column B of one full stripe is left
# out, and a write into column C of the same row, mounted degraded, is torn by
# raid56_crash_point=1: C lands, the parity does not.  Then B's sixteen blocks
# of that stripe are read back, and each must read what was acknowledged or
# fail:
#   ro   first a read-only degraded mount, which recovers nothing (on RAID6
#        with P's device left out as well, so that no second parity is left
#        to cross-check the rebuild)
#   1    a read-write degraded mount, whose recovery meets the torn stripe
#   2    on RAID6 once more with P's device left out as well
# Two shapes of the torn stripe:
#   plain  B was never written: every block must read 'A'
#   named  B's first block was overwritten with 'N's, fsync'd, while its device
#          was already gone -- acknowledged, the 'N's only in the parity, and
#          the log naming column B stale -- before the torn write into C.  The
#          usual shape of a stripe written into while degraded.  Block 0 must
#          read 'N', the rest 'A'.
#   fixed          the read-only mount refuses a rebuild no parity is left to
#                  check in a stripe the log lists, raises read_unrecovered
#                  and its raid56_health asks for a read-write mount; the
#                  recovery records the stripe as undecidable (every parity
#                  still there stale), named or not: B's blocks fail, none
#                  reads wrong
#   control        btrfs.raid56_recover_absent_legacy=1 (the old recovery),
#                  btrfs.raid56_read_trusts_unrecovered=1 (the old read-only
#                  mount) and btrfs.raid56_read_trusts_torn=1 (the old read
#                  of a stripe the recovery kept possibly torn): B's first
#                  block reads back as the rebuild from the torn parity, 'C's
#                  with no error -- on RAID5 at once, on RAID6 once P is gone
#                  too (before that the read path's own Q cross-check refuses
#                  it)
#   named-control  btrfs.raid56_recover_absent_skip_named=1 (and
#                  btrfs.raid56_read_trusts_torn=1): the recovery classifies
#                  only stripes whose record names no member, so the named
#                  stripe goes down the old path and B's first block reads
#                  back as 'N' xor the change, with no error
#   ro-control     on RAID5, the read-only mount once more with
#                  btrfs.raid56_wf_unrecovered_as_ambiguous=1: the refusal is
#                  counted as read_unverifiable, whose explanation sends the
#                  administrator to a scrub, and raid56_health reads ok
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: degraded_crash.sh <kernel>}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-dcr.sh.$$ && mv -f $T/umltest/init-dcr.sh.$$ $T/umltest/init-dcr.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
boot() {	# tag ndev profile mode omit opts [kernel args...]
	local tag=$1 ndev=$2 profile=$3 mode=$4 omit=$5 opts=$6 ubds="" mnt="" d a ph=""
	local D=$T/umltest/$tag
	shift 6
	for a in "$@"; do case $a in PHASE=*) ph=.${a#PHASE=};; esac; done
	for d in $(seq 0 $((ndev-1))); do
		case " $omit " in *" $d "*) continue;; esac
		ubds="$ubds ubd$d=$D/disk$d.img"
		[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
	done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-dcr.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$mode OPTS=$opts PROFILE=$profile TAG=$tag \
		NDEV=$ndev MNTDEV=$mnt "$@" < /dev/null \
		> $D/log.$mode.$opts.omit${omit// /-}$ph 2>&1
	echo "boot $mode $opts omit=$omit rc=$?" >> $D/log.boots
}
arm() {	# name ndev profile named [kernel args...]
	local tag=dcr-$1 ndev=$2 profile=$3 named=$4
	local D=$T/umltest/$tag idx_b idx_p ro_omit
	shift 4
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/layout.$tag $T/umltest/dcr.$tag.* $T/umltest/results.$tag
	for i in $(seq 0 $((ndev-1))); do truncate -s 1G $D/disk$i.img; done
	boot $tag $ndev $profile degraded_crash_prep "" rw
	idx_b=$(sed -n 's/.*IDX_B=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	idx_p=$(sed -n 's/.*IDX_P=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	if [ -n "$idx_b" ] && [ -n "$idx_p" ]; then
		boot $tag $ndev $profile degraded_crash "$idx_b" rw CRASH=1 \
			NAMED=$named "$@"
		# Read-only first: it writes nothing, so the phases after it
		# meet the stripe exactly as the crash left it.
		if [ "$named" = 0 ]; then
			ro_omit=$idx_b
			[ "$ndev" -ge 4 ] && ro_omit="$idx_b $idx_p"
			boot $tag $ndev $profile degraded_crash_read "$ro_omit" ro \
				PHASE=ro NAMED=0 "$@"
			[ $tag = dcr-r5-fixed ] &&
				boot $tag $ndev $profile degraded_crash_read "$ro_omit" ro \
					PHASE=roctl NAMED=0 \
					btrfs.raid56_wf_unrecovered_as_ambiguous=1 "$@"
		fi
		boot $tag $ndev $profile degraded_crash_read "$idx_b" rw PHASE=1 \
			NAMED=$named "$@"
		[ "$ndev" -ge 4 ] &&
			boot $tag $ndev $profile degraded_crash_read "$idx_b $idx_p" rw \
				PHASE=2 NAMED=$named "$@"
	fi
	rm -f $D/disk*.img
}
CTL="btrfs.raid56_recover_absent_legacy=1 btrfs.raid56_read_trusts_unrecovered=1"
CTL="$CTL btrfs.raid56_read_trusts_torn=1"
NCTL="btrfs.raid56_recover_absent_skip_named=1 btrfs.raid56_read_trusts_torn=1"
ARMS="r5-fixed r5-control r6-fixed r6-control"
ARMS="$ARMS r5-named-fixed r5-named-control r6-named-fixed r6-named-control"
arm r5-fixed 3 raid5:raid1 0
arm r5-control 3 raid5:raid1 0 $CTL
arm r6-fixed 4 raid6:raid1c3 0
arm r6-control 4 raid6:raid1c3 0 $CTL
arm r5-named-fixed 3 raid5:raid1 1
arm r5-named-control 3 raid5:raid1 1 $NCTL
arm r6-named-fixed 4 raid6:raid1c3 1
arm r6-named-control 4 raid6:raid1c3 1 $NCTL
SHOW="layout:|crash armed|NO_CRASH|crash injection|DCR |WRONG|torn by a crash"
SHOW="$SHOW|named write|NAMED_WRITE_FAIL|possibly torn|read-only mount does not"
SHOW="$SHOW|parities disagree|no parity is left|REFUSED a read|refusing a read"
SHOW="$SHOW|CHATTR_FAIL|LAYOUT_FAIL|KERNEL_SPLAT|MOUNT_FAIL|MKFS_FAIL"
for a in $ARMS; do
	grep -ahE "$SHOW" $T/umltest/dcr-$a/log.* 2>/dev/null | sed "s/^/  [$a] /"
done
# Each result file: ok eio wrong recovery_suspect read_unverifiable
res() { cat $T/umltest/dcr.dcr-$1.$2 2>/dev/null || echo "? ? ? ? ?"; }
read -r _ f5_eio f5_wrong f5_sus _ <<<"$(res r5-fixed 1)"
read -r _ _ c5_wrong _ _ <<<"$(res r5-control 1)"
read -r _ _ f6_wrong f6_sus _ <<<"$(res r6-fixed 1)"
read -r _ _ f6b_wrong _ _ <<<"$(res r6-fixed 2)"
read -r _ _ c6_wrong _ _ <<<"$(res r6-control 1)"
read -r _ _ c6b_wrong _ _ <<<"$(res r6-control 2)"
read -r _ f5r_eio f5r_wrong _ _ <<<"$(res r5-fixed ro)"
# The read-only phases' alerts: read_unrecovered read_unverifiable state action
hres() { cat $T/umltest/dcrh.dcr-$1.$2 2>/dev/null || echo "? ? ? ?"; }
read -r f5r_unrec _ f5r_state f5r_action <<<"$(hres r5-fixed ro)"
read -r c5r_unrec c5r_amb c5r_state _ <<<"$(hres r5-fixed roctl)"
read -r _ _ c5r_wrong _ _ <<<"$(res r5-control ro)"
read -r _ f6r_eio f6r_wrong _ _ <<<"$(res r6-fixed ro)"
read -r _ _ c6r_wrong _ _ <<<"$(res r6-control ro)"
read -r _ n5_eio n5_wrong n5_sus _ <<<"$(res r5-named-fixed 1)"
read -r _ _ nc5_wrong _ _ <<<"$(res r5-named-control 1)"
read -r _ _ n6_wrong n6_sus _ <<<"$(res r6-named-fixed 1)"
read -r _ _ n6b_wrong _ _ <<<"$(res r6-named-fixed 2)"
read -r _ _ nc6_wrong _ _ <<<"$(res r6-named-control 1)"
read -r _ _ nc6b_wrong _ _ <<<"$(res r6-named-control 2)"
echo "  column B blocks read back wrong without an error:"
echo "    read-only  RAID5 fixed $f5r_wrong control $c5r_wrong;" \
     "RAID6 (P out too) fixed $f6r_wrong control $c6r_wrong"
echo "  read-only RAID5 refusals: fixed read_unrecovered $f5r_unrec, health $f5r_state," \
     "action $f5r_action; ro-control read_unrecovered $c5r_unrec read_unverifiable" \
     "$c5r_amb, health $c5r_state"
echo "    plain      RAID5 fixed $f5_wrong control $c5_wrong"
echo "    plain      RAID6 fixed $f6_wrong/$f6b_wrong control $c6_wrong/$c6b_wrong" \
     "(B missing / P missing too)"
echo "    named      RAID5 fixed $n5_wrong control $nc5_wrong"
echo "    named      RAID6 fixed $n6_wrong/$n6b_wrong control $nc6_wrong/$nc6b_wrong" \
     "(B missing / P missing too)"
ALL="$f5_wrong$c5_wrong$f6_wrong$f6b_wrong$c6_wrong$c6b_wrong"
ALL="$ALL$f5r_wrong$c5r_wrong$f6r_wrong$c6r_wrong"
ALL="$ALL$n5_wrong$nc5_wrong$n6_wrong$n6b_wrong$nc6_wrong$nc6b_wrong"
ALL="$ALL$f5r_unrec$f5r_state$c5r_unrec$c5r_amb$c5r_state"
case "$ALL" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
for a in $ARMS; do
	grep -aq "crash injection 1 at full stripe" $T/umltest/dcr-$a/log.degraded_crash.* \
		2>/dev/null && continue
	echo "RESULT: INCONCLUSIVE -- $a never crashed in the torn write"
	exit 2
done
for a in r5-named-fixed r5-named-control r6-named-fixed r6-named-control; do
	grep -aq "named write acknowledged" $T/umltest/dcr-$a/log.degraded_crash.* \
		2>/dev/null && continue
	echo "RESULT: INCONCLUSIVE -- $a: the degraded write into B was not acknowledged"
	exit 2
done
if grep -alq KERNEL_SPLAT $T/umltest/dcr-*/log.* 2>/dev/null; then
	echo "RESULT: FAIL -- kernel splat"
	exit 1
fi
# A control that reads nothing wrong makes a clean fixed arm prove nothing.
for c in "c5_wrong:RAID5 recovery" "c6b_wrong:RAID6 recovery" \
	 "c5r_wrong:RAID5 read-only" "c6r_wrong:RAID6 read-only" \
	 "nc5_wrong:RAID5 named" "nc6b_wrong:RAID6 named"; do
	v=${c%%:*}
	[ "${!v}" != 0 ] && continue
	echo "RESULT: INCONCLUSIVE -- the ${c#*:} control read nothing wrong"
	exit 2
done
for v in f5_wrong f6_wrong f6b_wrong f5r_wrong f6r_wrong n5_wrong n6_wrong n6b_wrong; do
	[ "${!v}" = 0 ] && continue
	echo "RESULT: FAIL -- a fixed arm let a wrong rebuild of the missing column" \
	     "be read ($v=${!v})"
	exit 1
done
if [ "$f5_eio" -lt 1 ] || [ "$n5_eio" -lt 1 ]; then
	echo "RESULT: FAIL -- RAID5 fixed read the torn block neither wrong nor as EIO"
	exit 1
fi
if [ "$f5r_eio" -lt 1 ] || [ "$f6r_eio" -lt 1 ]; then
	echo "RESULT: FAIL -- the read-only mount read the torn block neither wrong nor as EIO"
	exit 1
fi
if [ "$c5r_state" != ok ] || [ "$c5r_amb" -lt 1 ]; then
	echo "RESULT: INCONCLUSIVE -- the read-only control did not show the old alert" \
	     "(read_unverifiable $c5r_amb, health $c5r_state)"
	exit 2
fi
if [ "$f5r_unrec" -lt 1 ] || [ "$f5r_state" = ok ] || [ "$f5r_action" != mount-rw ]; then
	echo "RESULT: FAIL -- the read-only mount refused without its alert" \
	     "(read_unrecovered $f5r_unrec, health $f5r_state, action $f5r_action)"
	exit 1
fi
if [ "$f5_sus" -lt 1 ] || [ "$f6_sus" -lt 1 ] || [ "$n5_sus" -lt 1 ] ||
   [ "$n6_sus" -lt 1 ]; then
	echo "RESULT: FAIL -- nothing read wrong, but the recovery recorded no stripe undecidable"
	echo "        (recovery_suspect: plain $f5_sus on RAID5, $f6_sus on RAID6;" \
	     "named $n5_sus on RAID5, $n6_sus on RAID6)"
	exit 1
fi
echo "RESULT: PASS -- after a crash while degraded the missing column reads correct or EIO,"
echo "        on a read-only mount and after the recovery, whether or not the log named it;"
echo "        the old code handed back its rebuild from the torn parity"
echo "        (read-only: $c5r_wrong block(s) on RAID5, $c6r_wrong on RAID6;" \
     "recovery: $c5_wrong on RAID5, $c6b_wrong on RAID6 once P was gone;" \
     "named: $nc5_wrong on RAID5, $nc6b_wrong on RAID6); the read-only refusal"
echo "        is read_unrecovered with health $f5r_state (action $f5r_action), where the"
echo "        old alert left it $c5r_state"
