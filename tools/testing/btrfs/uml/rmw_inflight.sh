#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# A write into a full stripe whose record names a column stale first writes
# that column back (rmw_repair_first()).  Does a crash before that write-back
# goes out, or inside it, leave the column's acknowledged data unreadable for
# good -- although the parity still holds it -- because the full stripe had
# already been recorded in flight?
#
#   rmw_inflight.sh <kernel> [before|inside...]
#
# See torn_present_prep, torn_present_fault and torn_present_read in
# init-final3.sh.  RAID5 on three devices, all present throughout, a nodatacow
# file of 'A's; B and C are data columns 0 and 1 of its first whole full
# stripe.  B's device fails the write of B's block 0 ('N's, acknowledged: the
# log names B stale, the 'N's only in P), then heals.  Then B's block 5 is
# overwritten in place with 'C's -- a write into the named column -- with
# raid56_crash_point armed:
#   before   5: the kernel panics before the write-back of B goes out
#   inside   6: it panics once the write-back of B has landed, before the
#            record says so
# Then two read-write mounts: the first reads columns B and C once its
# recovery is done, the second after a scrub.
#   fixed    the stripe is recorded in flight only once the write-back is
#            durable: the recovery finds B named and nothing possibly torn,
#            rebuilds B from P, and every block reads what was acknowledged
#            ('N' at block 0, 'A' or 'C' at block 5, 'A' elsewhere); B's block
#            0 on the platter is 'N'
#   control  raid56_rmw_mark_before_repair=1 on the boot that crashes: the
#            stripe was recorded in flight first, the recovery cannot decide
#            it (torn_undecidable), and B reads as EIO in both mounts
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: rmw_inflight.sh <kernel> [before|inside...]}
shift
SCEN=${*:-before inside}
NDEV=3
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rif.sh.$$ &&
	mv -f $T/umltest/init-rif.sh.$$ $T/umltest/init-rif.sh
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
boot() {	# tag mode phase [args...]
	local tag=$1 mode=$2 phase=$3 ubds="" d
	local D=$T/umltest/$tag
	shift 3
	for d in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$d=$D/disk$d.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rif.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw PROFILE=$PROFILE TAG=$tag \
		NDEV=$NDEV MNTDEV=/dev/ubda PHASE=$phase "$@" < /dev/null \
		> $D/log.$phase 2>&1
	echo "boot $mode $phase rc=$?" >> $D/log.boots
}
lay() {	# tag var
	sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" $T/umltest/layout.$1 2>/dev/null
}
crash_of() {	# scenario
	case $1 in before) echo 5;; inside) echo 6;; esac
}
arm() {	# scenario name [crashing-boot kernel args...]
	local scen=$1 tag=rif-$1-$2 idx phys
	local D=$T/umltest/rif-$1-$2
	shift 2
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/layout.$tag $T/umltest/tp.$tag.* $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot $tag torn_present_prep prep
	if [ -n "$(lay $tag FO_B)" ]; then
		boot $tag torn_present_fault fault FAILDEV=B WRITECOL=B WROW=0 \
			TORN=B ROW=5 CRASH=$(crash_of $scen) "$@"
		boot $tag torn_present_read rw1 READCOLS=B_C WRITECOL=B WROW=0 TORN=B ROW=5
		boot $tag torn_present_read rw2 READCOLS=B_C WRITECOL=B WROW=0 TORN=B ROW=5 \
			SCRUB=1
	fi
	# 'N' bytes of B's block 0 as its device's image holds it.
	idx=$(lay $tag IDX_B); phys=$(lay $tag PHYS_B)
	if [ -n "$idx" ] && [ -n "$phys" ]; then
		dd if=$D/disk$idx.img bs=4096 skip=$((phys / 4096)) count=1 status=none \
			2>/dev/null | tr -cd 'N' | wc -c > $D/platter
	fi
	rm -f $D/disk*.img
}
for s in $SCEN; do
	case $s in
	before|inside)
		arm $s fixed
		arm $s control btrfs.raid56_rmw_mark_before_repair=1;;
	*) echo "unknown scenario $s"; exit 2;;
	esac
done
SHOW="layout:|crash armed|NO_CRASH|crash injection|TP |TP_WRONG|named write|NAMED_WRITE_FAIL"
SHOW="$SHOW|could not decide|cannot be decided|REFUSED a read|refusing a read|refusing a write"
SHOW="$SHOW|CHATTR_FAIL|LAYOUT_FAIL|KERNEL_SPLAT|MOUNT_FAIL|MKFS_FAIL|healed device|scrub rc"
for d in $T/umltest/rif-*; do
	[ -d $d ] || continue
	a=${d##*/rif-}
	case " $SCEN " in *" ${a%%-*} "*) ;; *) continue;; esac
	grep -ahE "$SHOW" $d/log.* 2>/dev/null | sed "s/^/  [$a] /"
	[ -f $d/platter ] &&
		echo "  [$a] platter: $(cat $d/platter) of 4096 bytes 'N' in B's block 0"
done
val() {	# arm phase key
	local v
	v=$(sed -n "s/.* $3=\([^ ]*\).*/\1/p" $T/umltest/tp.rif-$1.$2 2>/dev/null)
	echo "${v:-?}"
}
plat() { cat $T/umltest/rif-$1/platter 2>/dev/null || echo "?"; }
sum() {	# counts...: their sum, or ? if one is missing
	local n=0 v
	for v in "$@"; do
		case $v in ''|*[!0-9]*) echo "?"; return;; esac
		n=$((n + v))
	done
	echo $n
}
fail=0; inconc=0; pass=""
verdict() {	# FAIL|INCONCLUSIVE message...
	echo "RESULT: $1 -- ${*:2}"
	[ "$1" = FAIL ] && fail=1
	[ "$1" = INCONCLUSIVE ] && inconc=1
}
for s in $SCEN; do
	grep -alq KERNEL_SPLAT $T/umltest/rif-$s-*/log.* 2>/dev/null || continue
	echo "RESULT: FAIL -- kernel splat ($s)"
	exit 1
done
# Did every arm name B, crash where it was told to, and finish and report --
# each count a number?  A control that never ran reproduced nothing, and a
# fixed arm that never ran proves nothing either: INCONCLUSIVE.  A fixed arm
# that hung: FAIL.
reported() {	# scenario count...
	local s=$1 a v cp
	shift
	cp=$(crash_of $s)
	for a in fixed control; do
		if grep -aq WATCHDOG $T/umltest/rif-$s-$a/log.* 2>/dev/null ||
		   grep -aq 'rc=124$' $T/umltest/rif-$s-$a/log.boots 2>/dev/null; then
			[ $a = fixed ] && { verdict FAIL "$s: the fixed arm hung"; return 1; }
			verdict INCONCLUSIVE "$s: the $a arm hung"
			return 1
		fi
		grep -aq "named write acknowledged" $T/umltest/rif-$s-$a/log.fault 2>/dev/null || {
			verdict INCONCLUSIVE "$s: the $a arm's write naming B was not acknowledged"
			return 1
		}
		grep -aq "crash injection $cp .* the write-back of full stripe" \
			$T/umltest/rif-$s-$a/log.fault 2>/dev/null || {
			verdict INCONCLUSIVE "$s: the $a arm never crashed at point $cp"
			return 1
		}
		if grep -aqE "LAYOUT_FAIL|MOUNT_FAIL|MKFS_FAIL|CHATTR_FAIL" \
			$T/umltest/rif-$s-$a/log.* 2>/dev/null ||
		   [ ! -s $T/umltest/tp.rif-$s-$a.rw1 ] ||
		   [ ! -s $T/umltest/tp.rif-$s-$a.rw2 ]; then
			verdict INCONCLUSIVE "$s: the $a arm did not finish and report"
			return 1
		fi
	done
	for v in "$@"; do
		case $v in
		''|*[!0-9]*)
			verdict INCONCLUSIVE "$s: a count is missing ($*)"
			return 1;;
		esac
	done
}
for s in $SCEN; do
	fo1=$(val $s-fixed rw1 B_ok); fe1=$(val $s-fixed rw1 B_eio); fw1=$(val $s-fixed rw1 B_wrong)
	fo2=$(val $s-fixed rw2 B_ok); fe2=$(val $s-fixed rw2 B_eio); fw2=$(val $s-fixed rw2 B_wrong)
	fc=$(sum $(val $s-fixed rw1 C_ok) $(val $s-fixed rw2 C_ok))
	ft=$(val $s-fixed rw1 tund)
	co1=$(val $s-control rw1 B_ok); ce1=$(val $s-control rw1 B_eio)
	cw1=$(val $s-control rw1 B_wrong); co2=$(val $s-control rw2 B_ok)
	ce2=$(val $s-control rw2 B_eio); cw2=$(val $s-control rw2 B_wrong)
	ct=$(val $s-control rw1 tund)
	cc=$(sum $(val $s-control rw1 C_wrong) $(val $s-control rw2 C_wrong) \
		 $(val $s-fixed rw1 C_wrong) $(val $s-fixed rw2 C_wrong))
	fp=$(plat $s-fixed); cp=$(plat $s-control)
	echo "  $s: column B ok/eio/wrong of 16 after the recovery, after the scrub:" \
	     "fixed $fo1/$fe1/$fw1, $fo2/$fe2/$fw2; control $co1/$ce1/$cw1, $co2/$ce2/$cw2;" \
	     "column C read ok by the fixed arm $fc of 32, wrong in any arm $cc;" \
	     "torn_undecidable fixed $ft control $ct;" \
	     "B's block 0 on the platter 'N' bytes fixed $fp control $cp"
	reported $s "$fo1" "$fe1" "$fw1" "$fo2" "$fe2" "$fw2" "$fc" "$ft" "$co1" "$ce1" \
		"$cw1" "$co2" "$ce2" "$cw2" "$ct" "$cc" "$fp" "$cp" || continue
	if [ "$fw1" != 0 ] || [ "$fw2" != 0 ] || [ "$cw1" != 0 ] || [ "$cw2" != 0 ] ||
	   [ "$cc" != 0 ]; then
		verdict FAIL "$s: a block was read back wrong with no error"
	elif [ "$ce1" = 0 ] || [ "$ce2" = 0 ] || [ "$ct" = 0 ]; then
		verdict INCONCLUSIVE "$s: the control kept B readable (eio $ce1/$ce2," \
			"torn_undecidable $ct): nothing reproduced"
	elif [ "$fo1" != 16 ] || [ "$fo2" != 16 ] || [ "$fc" != 32 ]; then
		verdict FAIL "$s: the fixed arm did not read back what was acknowledged" \
			"(B ok $fo1/$fo2 of 16, eio $fe1/$fe2; C ok $fc of 32)"
	elif [ "$ft" != 0 ] || [ "$fp" != 4096 ]; then
		verdict FAIL "$s: the fixed arm's recovery did not rebuild B" \
			"(torn_undecidable $ft, 'N' on the platter $fp)"
	else
		pass="$pass $s"
	fi
done
[ $fail = 1 ] && exit 1
[ $inconc = 1 ] && exit 2
echo "RESULT: PASS -- a crash before or inside the write-back of the named column left"
echo "        it readable, rebuilt from P ($pass ); recorded in flight first, the"
echo "        control read it as EIO after the recovery and after a scrub"
