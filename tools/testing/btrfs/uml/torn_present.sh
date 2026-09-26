#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# A full stripe a write may have torn, with no parity left over to check a
# rebuild the recovery or a read would make out of it: is that rebuild made,
# written back or handed back as data, and is the stripe the recovery cannot
# decide alerted?
#
#   torn_present.sh <kernel> [named|classify|unreadable...]
#
# See torn_present_prep, torn_present_fault and torn_present_read in
# init-final3.sh.  A nodatacow file of 'A's; B, C, D are data columns 0, 1, 2
# of its first whole full stripe.  Each scenario runs as a fixed arm and as
# controls, each a kernel knob that restores what the code did before:
#
#   named       RAID5, three devices, all present throughout.  B's device
#               fails the write of B's block 0 ('N's, acknowledged: the log
#               names B stale, the 'N's only in P), then heals; a write into
#               C's block 5 is torn by raid56_crash_point=1 (C lands, P does
#               not), with B's repair written beside it and B still named in
#               the log -- what a failed flush naming a column on a write in
#               flight leaves (see SINGLE below).  The read-write mount's
#               recovery meets a possibly torn stripe naming a column that P
#               alone can rebuild.
#     fixed     nothing is rebuilt from P, the stripe is recorded undecidable
#               (torn_undecidable): B reads as EIO, B's block 5 on the platter
#               is still 'A'
#     control   raid56_scrub_torn_trusts_parity=1: B is rebuilt from P and
#               written back -- block 5 becomes 'C', read back with no error
#     alertctl  raid56_recover_suspect_silent=1: recorded undecidable and
#               read as EIO as in the fixed arm, but with no alert
#   classify    RAID6, five devices.  The same with C named and D torn, then
#               mounted with B's device left out: the classification of a
#               stripe with a data column on a missing device, whose verify
#               pass used to rebuild C from both parities and write it back
#               before deciding anything.  Scored on C's block 5 on the platter.
#     fixed     'A'
#     4only     raid56_scrub_torn_trusts_parity=1 alone: still 'A', the verify
#               pass acts on no record
#     control   and raid56_recover_absent_verify_rebuilds=1: not 'A'
#   unreadable  RAID5, three devices, all present: a write into C's block 5
#               torn by the crash, nothing named; at the read-write mount B's
#               block 5 does not read.  The recovery keeps the stripe
#               undecided, marked possibly torn, and then a read of B's
#               block 5 has to rebuild it from P alone.
#     fixed     EIO (read_unrecovered)
#     control   raid56_read_trusts_torn=1: the rebuild, 'C's, with no error
#   readd       RAID5, three devices, all present: the unreadable arm's
#               stripe, kept torn by the recovery; then B's block reads
#               again, B's block 0 is overwritten with 'N's and B's device
#               fails the next commit's barrier: the readd names B and asks
#               for a repair of the stripe, which would rebuild B from P
#               alone and write it back -- 'C's over B's block 5.
#     fixed     the repair is refused (stripe_undecidable): B's block 5 on
#               the platter is still 'A', and B reads as EIO
#     control   raid56_rmw_trusts_torn=1: the repair writes the rebuild
#               back and drops the name: B's block 5 reads 'C', no error
#   fault       the same, with B's device failing the write of the 'N's
#               itself: named on the fault, and repaired on it
#   evict       RAID5, three devices, all present: the unreadable arm's
#               stripe, kept torn by the recovery, B's block 5 still not
#               reading; then C's device throws its writes away under a
#               write into each of 164 regions of a second nodatacow file
#               and fails a commit's barrier: the readd takes them back
#               possibly torn, too many to name, and the log is full of
#               records that say no more than that -- the recovery's first
#               in the table.  One write into a new region spends one.
#     fixed     one a failed flush left: B's block 5 still reads as EIO
#     control   raid56_wf_kept_torn_in_order=1: the recovery's, spent in
#               table order, and B's block 5 reads back the rebuild from
#               P, 'C's, with no error
#   misspar     RAID6, four devices: the same torn write, then a read-write
#               mount without the stripe's Q device.  The recovery
#               regenerates P from the data and keeps the stripe recorded
#               only for Q; once it is done, B's block 5 stops reading, and
#               its read has to rebuild it from that P alone.
#     fixed     'A', the block as it is on the disk: Q is recorded stale,
#               the stripe no longer possibly torn
#     control   raid56_wf_missing_parity_keeps_torn=1: kept possibly torn,
#               EIO (read_unrecovered) for a rebuild that was exact
#   decided     the same, with the torn column C's device left out instead:
#               both parities agree about C (the crash dropped their
#               writes), so the recovery decides C and regenerates both;
#               once it is done, B's block 5 stops reading, and its read
#               has to rebuild B and C from P and Q.
#     fixed     'A': the stripe kept recorded for C's device, no longer
#               possibly torn
#     control   raid56_wf_absent_decided_keeps_torn=1: kept possibly torn,
#               EIO (read_unrecovered) for a rebuild that was exact
#   runtime     unreadable's recovery, then B's device heals and fails only
#               the write of B's block 0 ('N's, acknowledged): a stripe
#               marked possibly torn at runtime that names B, which only P
#               can rebuild.  B's block 0 is read, a scrub run, and it is
#               read again.
#     fixed     EIO both times; the refusal says no scrub can decide the
#               stripe and how to clear it, and the scrub that leaves it
#               untouched raises torn_undecidable
#     control   raid56_wf_torn_remedy_legacy=1: the refusal promises that
#               a scrub makes the reads work again -- the scrub leaves the
#               stripe untouched, the read still fails -- and nothing but a
#               rate limited warning says so
#   clear       named's torn write, then one mount whose recovery records
#               the stripe undecidable, every device present: an in-place
#               rewrite of B's block 1 is refused.  Then what the alert
#               says: the file deleted, sync, scrub; a new nodatacow file
#               preallocated over the same stripe, its blocks in column B
#               written in place (a read-modify-write each) and read back.
#     fixed     every write lands and reads back; the refusal named that
#               sequence
#     control   raid56_scrub_empty_keeps_record=1 and
#               raid56_wf_torn_remedy_legacy=1: the scrub keeps the record
#               of the emptied stripe, the new file's writes fail with EIO
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: torn_present.sh <kernel> [named|classify|unreadable|readd|fault|evict|misspar|decided|runtime|clear...]}
shift
SCEN=${*:-named classify unreadable readd fault evict misspar decided runtime clear}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-tp.sh.$$ &&
	mv -f $T/umltest/init-tp.sh.$$ $T/umltest/init-tp.sh
for f in raid56_layout.py raid56_rows.py; do
	cp $HERE/$f $T/umltest/$f.$$ && mv -f $T/umltest/$f.$$ $T/umltest/$f
done
ulimit -c 0
# The torn write of named and classify has to leave the log naming the column
# it would otherwise repair first (rmw_repair_first()): the state a failed
# flush's name on a write in flight leaves (@hold), which the rig cannot time
# -- the barrier failure would have to land inside the torn write's flight.
# Writing the repair in the same batch as the parity gives the same log and
# platters: the named column repaired on its device, still named in the log,
# the parity of the torn row not written.
SINGLE=btrfs.raid56_rmw_single_phase=1
boot() {	# tag ndev profile mode phase omit opts [args...]
	local tag=$1 ndev=$2 profile=$3 mode=$4 phase=$5 omit=$6 opts=$7 ubds="" mnt="" d
	local D=$T/umltest/$tag
	shift 7
	for d in $(seq 0 $((ndev-1))); do
		[ "$d" = "$omit" ] && continue
		ubds="$ubds ubd$d=$D/disk$d.img"
		[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
	done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-tp.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$mode OPTS=$opts PROFILE=$profile TAG=$tag \
		NDEV=$ndev MNTDEV=$mnt OMITTED=$([ "$omit" = none ] || echo "$omit") \
		PHASE=$phase "$@" < /dev/null \
		> $D/log.$phase 2>&1
	echo "boot $mode $phase omit=$omit rc=$?" >> $D/log.boots
}
lay() {	# tag var
	sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" $T/umltest/layout.$1 2>/dev/null
}
# 'A' bytes of block <row> of column <col> as its device's image holds it.
platter() {	# tag col row
	local idx phys
	idx=$(lay $1 IDX_$2); phys=$(lay $1 PHYS_$2)
	[ -n "$idx" ] && [ -n "$phys" ] || { echo "?"; return; }
	dd if=$T/umltest/$1/disk$idx.img bs=4096 skip=$((phys / 4096 + $3)) count=1 \
		status=none 2>/dev/null | tr -cd 'A' | wc -c
}
arm() {	# scenario name [read-boot kernel args...]
	local scen=$1 tag=tp-$1-$2 ndev profile omit="" fault read ropts=rw rmode=torn_present_read
	local D=$T/umltest/tp-$1-$2
	shift 2
	case $scen in
	named)	ndev=3; profile=raid5:raid1
		fault="FAILDEV=B WRITECOL=B WROW=0 TORN=C ROW=5 CRASH=1 $SINGLE"
		read="READCOLS=B_C WRITECOL=B WROW=0 TORN=C ROW=5";;
	classify) ndev=5; profile=raid6:raid1c3
		fault="FAILDEV=C WRITECOL=C WROW=0 TORN=D ROW=5 CRASH=1 $SINGLE"
		read="READCOLS=C_D WRITECOL=C WROW=0 TORN=D ROW=5";;
	unreadable) ndev=3; profile=raid5:raid1
		fault="TORN=C ROW=5 CRASH=1"
		read="READCOLS=B_C TORN=C ROW=5 BAD=B";;
	readd|fault) ndev=3; profile=raid5:raid1
		fault="TORN=C ROW=5 CRASH=1"
		read="READCOLS=B_C TORN=C ROW=5 BAD=B WRITECOL=B WROW=0 NAMECOL=B"
		read="$read NAMEBY=$([ $scen = readd ] && echo flush || echo write)";;
	evict)	ndev=3; profile=raid5:raid1
		fault="TORN=C ROW=5 CRASH=1"
		read="READCOLS=B_C TORN=C ROW=5 BAD=B FILL=164 FILLDEV=C"
		# No transaction commit while C's device drops writes.
		ropts=rw,commit=600;;
	misspar|decided) ndev=4; profile=raid6:raid1c3
		fault="TORN=C ROW=5 CRASH=1"
		read="READCOLS=B_C TORN=C ROW=5 BAD=B BADAFTER=1";;
	runtime) ndev=3; profile=raid5:raid1
		fault="TORN=C ROW=5 CRASH=1"
		read="ROW=5"; rmode=torn_present_runtime;;
	clear)	ndev=3; profile=raid5:raid1
		fault="FAILDEV=B WRITECOL=B WROW=0 TORN=C ROW=5 CRASH=1 $SINGLE"
		read="ROW=5"; rmode=torn_present_clear;;
	esac
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/layout.$tag $T/umltest/tp.$tag.* $T/umltest/results.$tag
	for i in $(seq 0 $((ndev-1))); do truncate -s 1G $D/disk$i.img; done
	boot $tag $ndev $profile torn_present_prep prep none rw
	[ $scen = classify ] && omit=$(lay $tag IDX_B)
	[ $scen = misspar ] && omit=$(lay $tag IDX_Q)
	[ $scen = decided ] && omit=$(lay $tag IDX_C)
	if [ -n "$(lay $tag FO_B)" ]; then
		# shellcheck disable=SC2086
		boot $tag $ndev $profile torn_present_fault fault none rw $fault
		# shellcheck disable=SC2086
		boot $tag $ndev $profile $rmode rw "${omit:-none}" $ropts \
			$read "$@"
	fi
	case $scen in
	named|readd|fault) echo "$(platter $tag B 5)" > $D/platter;;
	classify) echo "$(platter $tag C 5)" > $D/platter;;
	esac
	rm -f $D/disk*.img
}
for s in $SCEN; do
	case $s in
	named)
		arm named fixed
		arm named control btrfs.raid56_scrub_torn_trusts_parity=1
		arm named alertctl btrfs.raid56_recover_suspect_silent=1;;
	classify)
		arm classify fixed
		arm classify 4only btrfs.raid56_scrub_torn_trusts_parity=1
		arm classify control btrfs.raid56_scrub_torn_trusts_parity=1 \
			btrfs.raid56_recover_absent_verify_rebuilds=1;;
	unreadable)
		arm unreadable fixed
		arm unreadable control btrfs.raid56_read_trusts_torn=1;;
	readd|fault)
		arm $s fixed
		arm $s control btrfs.raid56_rmw_trusts_torn=1;;
	evict)
		arm evict fixed
		arm evict control btrfs.raid56_wf_kept_torn_in_order=1;;
	misspar)
		arm misspar fixed
		arm misspar control btrfs.raid56_wf_missing_parity_keeps_torn=1;;
	decided)
		arm decided fixed
		arm decided control btrfs.raid56_wf_absent_decided_keeps_torn=1;;
	runtime)
		arm runtime fixed
		arm runtime control btrfs.raid56_wf_torn_remedy_legacy=1;;
	clear)
		arm clear fixed
		arm clear control btrfs.raid56_scrub_empty_keeps_record=1 \
			btrfs.raid56_wf_torn_remedy_legacy=1;;
	*) echo "unknown scenario $s"; exit 2;;
	esac
done
SHOW="layout:|crash armed|NO_CRASH|crash injection|TP |TP_WRONG|named write|NAMED_WRITE_FAIL"
SHOW="$SHOW|left untouched|could not decide|cannot be decided|REFUSED a read|refusing a read"
SHOW="$SHOW|unreadable on|CHATTR_FAIL|LAYOUT_FAIL|KERNEL_SPLAT|MOUNT_FAIL|MKFS_FAIL|DM_RELOAD"
SHOW="$SHOW|reads again|failed the barrier|healed device|refusing a repair|refusing a write"
SHOW="$SHOW|drops every write|fails writes and flushes|TP fill|TP_FILL_FAIL|TP_FRESH_FAIL|FALLOCATE_FAIL"
SHOW="$SHOW|recovery done|scrub rc|new layout"
for d in $T/umltest/tp-*; do
	[ -d $d ] || continue
	a=${d##*/tp-}
	case " $SCEN " in *" ${a%%-*} "*) ;; *) continue;; esac
	grep -ahE "$SHOW" $d/log.* 2>/dev/null | sed "s/^/  [$a] /"
	[ -f $d/platter ] && echo "  [$a] platter: $(cat $d/platter) of 4096 bytes 'A'"
done
val() {	# arm key
	local v
	v=$(sed -n "s/.* $2=\([^ ]*\).*/\1/p" $T/umltest/tp.tp-$1.rw 2>/dev/null)
	echo "${v:-?}"
}
plat() { cat $T/umltest/tp-$1/platter 2>/dev/null || echo "?"; }
fail=0; inconc=0; pass=""
verdict() {	# FAIL|INCONCLUSIVE message...
	echo "RESULT: $1 -- ${*:2}"
	[ "$1" = FAIL ] && fail=1
	[ "$1" = INCONCLUSIVE ] && inconc=1
}
for s in $SCEN; do
	grep -alq KERNEL_SPLAT $T/umltest/tp-$s-*/log.* 2>/dev/null || continue
	echo "RESULT: FAIL -- kernel splat ($s)"
	exit 1
done
# Did every arm of the scenario crash in the torn write, finish and report --
# each count a number?  A control that never ran reproduced nothing, and a
# fixed arm that never ran proves nothing either: INCONCLUSIVE.  A fixed arm
# that hung: FAIL.
reported() {	# scenario "arms" count...
	local s=$1 arms=$2 a v
	shift 2
	for a in $arms; do
		if grep -aq WATCHDOG $T/umltest/tp-$s-$a/log.* 2>/dev/null ||
		   grep -aq 'rc=124$' $T/umltest/tp-$s-$a/log.boots 2>/dev/null; then
			[ $a = fixed ] && { verdict FAIL "$s: the fixed arm hung"; return 1; }
			verdict INCONCLUSIVE "$s: the $a arm hung"
			return 1
		fi
		grep -aq "crash injection 1 at full stripe" $T/umltest/tp-$s-$a/log.fault \
			2>/dev/null || {
			verdict INCONCLUSIVE "$s: the $a arm's torn write never crashed"
			return 1
		}
		if grep -aqE "LAYOUT_FAIL|MOUNT_FAIL|MKFS_FAIL|CHATTR_FAIL|FALLOCATE_FAIL" \
			$T/umltest/tp-$s-$a/log.* 2>/dev/null ||
		   [ ! -s $T/umltest/tp.tp-$s-$a.rw ]; then
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
# Was B's block made unreadable after the mount in each arm?  If not, the
# fixed arm's clean read needed no rebuild at all.
unread() {	# scenario
	local a
	for a in fixed control; do
		grep -aq "unreadable on .*after the mount" $T/umltest/tp-$1-$a/log.rw && continue
		verdict INCONCLUSIVE "$1: the $a arm's block B was never made unreadable"
		return 1
	done
}
for s in $SCEN; do
	case $s in
	named)
		fw=$(val named-fixed B_wrong); fe=$(val named-fixed B_eio)
		fc=$(val named-fixed C_wrong); cw=$(val named-control B_wrong)
		fs=$(val named-fixed sus); ft=$(val named-fixed tund)
		fp=$(plat named-fixed); cp=$(plat named-control)
		aw=$(val named-alertctl B_wrong); as=$(val named-alertctl sus)
		at=$(val named-alertctl tund)
		echo "  named: column B read wrong fixed $fw (eio $fe) control $cw;" \
		     "B's block 5 on the platter 'A' bytes fixed $fp control $cp;" \
		     "recovery_suspect/torn_undecidable fixed $fs/$ft alertctl $as/$at"
		reported named "fixed control alertctl" "$fw" "$fe" "$fc" "$cw" "$fs" "$ft" \
			"$fp" "$cp" "$aw" "$as" "$at" || continue
		if [ "$cw" = 0 ] && [ "$cp" = 4096 ]; then
			verdict INCONCLUSIVE "named: the control read and wrote nothing wrong"
		elif ! [ "$as" -ge 1 ] 2>/dev/null || [ "$at" != 0 ] || [ "$aw" != 0 ]; then
			verdict INCONCLUSIVE "named: the alert control did not keep it silently" \
				"(recovery_suspect $as, torn_undecidable $at, wrong $aw)"
		elif [ "$fw" != 0 ] || [ "$fc" != 0 ] || [ "$fp" != 4096 ]; then
			verdict FAIL "named: the fixed arm rebuilt the named column from P"
		elif [ "$fs" -lt 1 ] 2>/dev/null || [ "$ft" -lt 1 ] 2>/dev/null ||
		     [ "$fe" -lt 1 ] 2>/dev/null; then
			verdict FAIL "named: the stripe was not recorded undecidable with an alert"
		else
			pass="$pass named"
		fi;;
	classify)
		fp=$(plat classify-fixed); op=$(plat classify-4only); cp=$(plat classify-control)
		fw=$(val classify-fixed C_wrong)$(val classify-fixed D_wrong)
		fs=$(val classify-fixed sus)
		echo "  classify: C's block 5 on the platter 'A' bytes fixed $fp 4only $op" \
		     "control $cp; fixed read wrong C+D $fw, recovery_suspect $fs"
		reported classify "fixed 4only control" "$fp" "$op" "$cp" "$fw" "$fs" || continue
		if [ "$cp" = 4096 ]; then
			verdict INCONCLUSIVE "classify: the control wrote nothing over C either"
		elif [ "$fp" != 4096 ] || [ "$op" != 4096 ]; then
			verdict FAIL "classify: a rebuild was written over C (fixed $fp, 4only $op)"
		elif [ "$fw" != 00 ] || [ "$fs" -lt 1 ] 2>/dev/null; then
			verdict FAIL "classify: read wrong $fw or not recorded undecidable ($fs)"
		else
			pass="$pass classify"
		fi;;
	unreadable)
		fw=$(val unreadable-fixed B_wrong); fe=$(val unreadable-fixed B_eio)
		fu=$(val unreadable-fixed unrec); cw=$(val unreadable-control B_wrong)
		fst=$(val unreadable-fixed state)
		echo "  unreadable: column B read wrong fixed $fw (eio $fe, read_unrecovered $fu," \
		     "health $fst) control $cw"
		reported unreadable "fixed control" "$fw" "$fe" "$fu" "$cw" || continue
		if [ "$cw" = 0 ]; then
			verdict INCONCLUSIVE "unreadable: the control read nothing wrong"
		elif [ "$fw" != 0 ]; then
			verdict FAIL "unreadable: the fixed arm read a rebuild from the torn parity"
		elif [ "$fe" -lt 1 ] 2>/dev/null || [ "$fu" -lt 1 ] 2>/dev/null ||
		     [ "$fst" = ok ]; then
			verdict FAIL "unreadable: the refusal raised no read_unrecovered alert"
		else
			pass="$pass unreadable"
		fi;;
	readd|fault)
		fw=$(val $s-fixed B_wrong); fe=$(val $s-fixed B_eio)
		fu=$(val $s-fixed und); cw=$(val $s-control B_wrong)
		fp=$(plat $s-fixed); cp=$(plat $s-control)
		echo "  $s: column B read wrong fixed $fw (eio $fe, stripe_undecidable $fu)" \
		     "control $cw; B's block 5 on the platter 'A' bytes fixed $fp control $cp"
		reported $s "fixed control" "$fw" "$fe" "$fu" "$cw" "$fp" "$cp" || continue
		if grep -aq NAMED_WRITE_FAIL $T/umltest/tp-$s-*/log.rw; then
			verdict INCONCLUSIVE "$s: the write that gets the name failed"
		elif [ "$cw" = 0 ] && [ "$cp" = 4096 ]; then
			verdict INCONCLUSIVE "$s: the control's repair wrote nothing over B"
		elif [ "$fw" != 0 ] || [ "$fp" != 4096 ]; then
			verdict FAIL "$s: the fixed arm wrote a rebuild from P over B" \
				"(wrong $fw, platter $fp)"
		elif [ "$fe" -lt 1 ] 2>/dev/null || [ "$fu" -lt 1 ] 2>/dev/null; then
			verdict FAIL "$s: the refused repair raised no stripe_undecidable" \
				"or B did not read as EIO (eio $fe)"
		else
			pass="$pass $s"
		fi;;
	evict)
		fw=$(val evict-fixed B_wrong); fe=$(val evict-fixed B_eio)
		cw=$(val evict-control B_wrong); ce=$(val evict-control B_eio)
		fs=$(val evict-fixed spent); cs=$(val evict-control spent)
		echo "  evict: column B read wrong fixed $fw (eio $fe) control $cw (eio $ce);" \
		     "records the write into a new region spent fixed $fs control $cs" \
		     "(torn_blocks fixed $(val evict-fixed tb) control $(val evict-control tb))"
		reported evict "fixed control" "$fw" "$fe" "$cw" "$ce" "$fs" "$cs" || continue
		if [ "$fs" != 1 ] || [ "$cs" != 1 ]; then
			verdict INCONCLUSIVE "evict: the write into a new region did not spend exactly" \
				"one record (fixed $fs, control $cs): the log was not full of them"
		elif [ "$cw" = 0 ]; then
			verdict INCONCLUSIVE "evict: the control read nothing wrong"
		elif [ "$fw" != 0 ]; then
			verdict FAIL "evict: the recovery's record was spent first, B read a rebuild" \
				"from the torn parity"
		elif [ "$fe" -lt 1 ] 2>/dev/null; then
			verdict FAIL "evict: B's unreadable block was not refused (eio $fe)"
		else
			pass="$pass evict"
		fi;;
	misspar)
		fw=$(val misspar-fixed B_wrong); fe=$(val misspar-fixed B_eio)
		fu=$(val misspar-fixed unrec); cw=$(val misspar-control B_wrong)
		ce=$(val misspar-control B_eio); cu=$(val misspar-control unrec)
		fk=$(val misspar-fixed C_wrong)$(val misspar-fixed C_eio)
		echo "  misspar: column B read wrong/eio fixed $fw/$fe (read_unrecovered $fu)" \
		     "control $cw/$ce (read_unrecovered $cu); column C wrong+eio fixed $fk"
		reported misspar "fixed control" "$fw" "$fe" "$fu" "$cw" "$ce" "$cu" "$fk" ||
			continue
		unread $s || continue
		if [ "$cw" != 0 ]; then
			verdict FAIL "misspar: the control read a wrong rebuild"
		elif [ "$ce" = 0 ] || [ "$cu" = 0 ]; then
			verdict INCONCLUSIVE "misspar: the control refused nothing (eio $ce," \
				"read_unrecovered $cu)"
		elif [ "$fw" != 0 ] || [ "$fk" != 00 ]; then
			verdict FAIL "misspar: the fixed arm read a wrong rebuild"
		elif [ "$fe" != 0 ] || [ "$fu" != 0 ]; then
			verdict FAIL "misspar: the fixed arm refused a rebuild from the parity" \
				"the recovery regenerated (eio $fe, read_unrecovered $fu)"
		else
			pass="$pass misspar"
		fi;;
	decided)
		fw=$(val decided-fixed B_wrong); fe=$(val decided-fixed B_eio)
		fu=$(val decided-fixed unrec); cw=$(val decided-control B_wrong)
		ce=$(val decided-control B_eio); cu=$(val decided-control unrec)
		fk=$(val decided-fixed C_wrong)$(val decided-fixed C_eio)
		ck=$(val decided-control C_wrong)
		echo "  decided: column B read wrong/eio fixed $fw/$fe (read_unrecovered $fu)" \
		     "control $cw/$ce (read_unrecovered $cu); column C wrong+eio fixed $fk"
		reported decided "fixed control" "$fw" "$fe" "$fu" "$cw" "$ce" "$cu" "$fk" \
			"$ck" || continue
		unread $s || continue
		if [ "$cw" != 0 ] || [ "$ck" != 0 ]; then
			verdict FAIL "decided: the control read a wrong rebuild"
		elif [ "$ce" = 0 ] || [ "$cu" = 0 ]; then
			verdict INCONCLUSIVE "decided: the control refused nothing (eio $ce," \
				"read_unrecovered $cu)"
		elif [ "$fw" != 0 ] || [ "$fk" != 00 ]; then
			verdict FAIL "decided: the fixed arm read a wrong rebuild, or refused" \
				"one of column C (wrong+eio $fk)"
		elif [ "$fe" != 0 ] || [ "$fu" != 0 ]; then
			verdict FAIL "decided: the fixed arm refused a rebuild from the parities" \
				"the recovery regenerated (eio $fe, read_unrecovered $fu)"
		else
			pass="$pass decided"
		fi;;
	runtime)
		fb=$(val runtime-fixed b0); fbs=$(val runtime-fixed b0_scrubbed)
		fx=$(val runtime-fixed explain); ft=$(val runtime-fixed tund)
		cb=$(val runtime-control b0); cbs=$(val runtime-control b0_scrubbed)
		cx=$(val runtime-control explain); ct=$(val runtime-control tund)
		cl=$(val runtime-control scrub_left)
		echo "  runtime: B's block 0 before/after the scrub fixed $fb/$fbs" \
		     "control $cb/$cbs;" \
		     "explained fixed $fx control $cx; scrub left it untouched (control) $cl;" \
		     "torn_undecidable fixed $ft control $ct"
		reported runtime "fixed control" "$ft" "$ct" "$cl" || continue
		if grep -aq NAMED_WRITE_FAIL $T/umltest/tp-runtime-*/log.rw; then
			verdict INCONCLUSIVE "runtime: the write naming B was not acknowledged"
		elif [ "$fb$fbs$cb$cbs" != eioeioeioeio ]; then
			verdict FAIL "runtime: B's block 0 read other than EIO" \
				"(fixed $fb/$fbs, control $cb/$cbs)"
		elif [ "$cx" != promise ] || ! [ "$cl" -ge 1 ] 2>/dev/null || [ "$ct" != 0 ]; then
			verdict INCONCLUSIVE "runtime: the control did not promise the scrub that" \
				"then left the stripe untouched, silently ($cx, left $cl, tund $ct)"
		elif [ "$fx" != undecidable ] || ! [ "$ft" -ge 1 ] 2>/dev/null; then
			verdict FAIL "runtime: the refusal or the scrub did not say the stripe" \
				"cannot be decided (explained $fx, torn_undecidable $ft)"
		else
			pass="$pass runtime"
		fi;;
	clear)
		fw=$(val clear-fixed wr1); cw=$(val clear-control wr1)
		fs=$(val clear-fixed same); cs=$(val clear-control same)
		fe=$(val clear-fixed w_eio); ce=$(val clear-control w_eio)
		fr=$(val clear-fixed r_ok); fx=$(val clear-fixed explain)
		cx=$(val clear-control explain); fa=$(val clear-fixed action)
		echo "  clear: in-place rewrite fixed $fw control $cw; new file over the stripe" \
		     "fixed $fs control $cs; its writes EIO fixed $fe control $ce, read back" \
		     "fixed $fr of 16; explained fixed $fx control $cx; action fixed $fa"
		reported clear "fixed control" "$fs" "$cs" "$fe" "$ce" "$fr" || continue
		if [ "$fs" != 1 ] || [ "$cs" != 1 ]; then
			verdict INCONCLUSIVE "clear: the new file did not land on the emptied" \
				"stripe (fixed $fs, control $cs)"
		elif [ "$fw" != eio ] || [ "$cw" != eio ]; then
			verdict INCONCLUSIVE "clear: the in-place rewrite was not refused" \
				"(fixed $fw, control $cw)"
		elif ! [ "$ce" -ge 1 ] 2>/dev/null || [ "$cx" != old ]; then
			verdict INCONCLUSIVE "clear: the control's sequence did not fail" \
				"($ce EIO, explained $cx)"
		elif [ "$fe" != 0 ] || [ "$fr" != 16 ]; then
			verdict FAIL "clear: after the delete, sync and scrub the new file's" \
				"writes failed ($fe) or read back wrong ($fr of 16)"
		elif [ "$fx" != new ]; then
			verdict FAIL "clear: the refusal did not name the sequence that clears it"
		else
			pass="$pass clear"
		fi;;
	esac
done
[ $fail = 1 ] && exit 1
[ $inconc = 1 ] && exit 2
echo "RESULT: PASS -- no rebuild from a possibly torn parity without one left over to check"
echo "        it was written or read back as data ($pass ), each control did; the"
echo "        undecidable stripe raised torn_undecidable (named), stripe_undecidable (readd,"
echo "        fault) or read_unrecovered (unreadable), silent under the old code"
