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
# And one more boot:
#   platter  ro mount, but each acknowledged block is read straight off the
#            device btrfs-map-logical names (nocow_platter.py).  Not through
#            the filesystem: its read path consults the record and
#            reconstructs a recorded block, which is right for a reader and
#            useless for asking whether the recovery rewrote the column.
#
# Five arms:
#   fixed      recovery repairs: no record left, platters clean, parity clean
#   control    btrfs.raid56_recover_legacy=1, the old behaviour: records kept
#              and the platters still stale.  If it is clean the test does
#              not discriminate and proves nothing.
#   ambiguous  every parity recorded unusable, so the record cannot decide the
#              stripe: recovery must decline -- keep the record and leave the
#              parity that still holds the acknowledged value alone
#   quiet      the fixed arm with btrfs.raid56_repair_delay_ms=600000 on the
#              prep boot, so that no background repair runs before the
#              unmount: as clean as the fixed arm
#   inflight   the quiet arm with btrfs.raid56_wf_finished_stay_inflight=1 on
#              the prep boot: the log goes on listing in flight the
#              overwrites that finished -- each block written after a failed
#              barrier is a union with the last one, and the unmount commits
#              no transaction when the prep's sync left none running, so no
#              barrier after the heal drops them -- and the recovery takes
#              those stripes for writes a crash may have torn, and where the
#              named column needs the only parity it keeps them undecided
#              (SCRUB_WIB_TORN): records kept, platters still stale.  None of
#              those writes was in flight at a crash, and the quiet arm,
#              whose log says they finished, repairs them all.  Why quiet
#              and not fixed: a repair that lands after the heal flushes
#              every device (btrfs_wib_persist_now()) and drops the listing
#              anyway, a race of the prep that leaves this arm clean on some
#              runs -- and the fixed arm of a kernel without the fix too.
#   remount    the device goes on failing after the prep's last sync until
#              every repair queued on it has given up (raid56_repair_delay_ms
#              =100): each retry is a write recorded after the last
#              transaction commit -- a repair takes none -- into a stripe
#              whose record names the column, and it finishes, failed, with
#              the log still listing it in flight.  Then the heal, and a
#              remount read-only before the unmount, which then writes
#              nothing.  The remount writes the log as an unmount does: as
#              clean as the quiet arm
#   remountctl the remount arm with
#              btrfs.raid56_wf_remount_ro_keeps_inflight=1 on the prep boot:
#              the log the remount leaves lists the retries in flight (for
#              the next mount after a crash, or after the unmount, of the
#              read-only filesystem), and the recovery keeps their stripes
#              undecided as possibly torn.  If it keeps none, no retry was
#              left listed for the remount to drop: INCONCLUSIVE.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: recover_scrub.sh <kernel> [ndev] [fail-device]}
NDEV=${2:-4}
FAIL=${3:-1}
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-final3.sh.$$ && mv -f $T/umltest/init-final3.sh.$$ $T/umltest/init-final3.sh	# atomic: a guest may be reading it
cp $HERE/nocow_platter.py $T/umltest/nocow_platter.py.$$ &&
	mv -f $T/umltest/nocow_platter.py.$$ $T/umltest/nocow_platter.py
ulimit -c 0

MNTPROBE=/dev/ubda; [ "$FAIL" = "0" ] && MNTPROBE=/dev/ubdb

arm() {	# name legacy fakebadpar [prep kernel args]
	local tag=recover-scrub-$1 legacy=$2 fakebadpar=$3 prepargs="${4:-}" d
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

	boot nocow_persist_prep none /dev/mapper/d0 \
		"NOPERSIST=0 FAKEBADPAR=$fakebadpar${prepargs:+ $prepargs}"
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
	ACK=$(echo "$ACKED" | sed -n 's/.*: \([0-9]*\) of.*/\1/p')
	[ "${ACK:-0}" -gt 0 ] || NOACK="$NOACK $1"
	# RETRY_GIVE_UP: how many repairs gave up before the heal.
	GU=$(grep -ho 'repairs after .* gave_up=[0-9]*' $T/umltest/$tag/log.nocow_persist_prep.* \
		2>/dev/null | sed -n 's/.*gave_up=\([0-9]*\).*/\1/p' | tail -1)
	SPLAT=$(grep -l KERNEL_SPLAT $T/umltest/$tag/log.* 2>/dev/null | wc -l)
	# Full stripes the recovery kept undecided as possibly torn
	# (recovery_suspect: every device is there, so SCRUB_WIB_TORN).
	TORN=$(grep -ho 'after recovery: enabled.*' $T/umltest/$tag/log.nocow_recover.* \
		2>/dev/null | sed -n 's/.* recovery_suspect \([0-9]*\).*/\1/p' | head -1)
	echo "  $1: records after recovery=$STICKY  stale on platters=$DISK  lost from parity=$PAR  ($ACKED; splats=$SPLAT)"
	grep -hE 'recovery done|could not be resolved|left untouched|declin' \
		$T/umltest/$tag/log.nocow_recover.* 2>/dev/null | sed 's/^/      /' | head -4
}

KNOB=raid56_wf_finished_stay_inflight
QUIET=btrfs.raid56_repair_delay_ms=600000
arm fixed 0 0
arm control 1 0
arm ambiguous 0 1
arm quiet 0 0 "$QUIET"
arm inflight 0 0 "$QUIET btrfs.$KNOB=1"
RETRY="btrfs.raid56_repair_delay_ms=100 RETRY_GIVE_UP=1 REMOUNT_RO=1"
arm remount 0 0 "$RETRY"
arm remountctl 0 0 "$RETRY btrfs.raid56_wf_remount_ro_keeps_inflight=1"

echo "== results =="
NOACK=""
report fixed;     F_ST=$STICKY F_DISK=$DISK F_PAR=$PAR F_SPLAT=$SPLAT F_TORN=$TORN
report control;   C_ST=$STICKY C_DISK=$DISK C_PAR=$PAR C_SPLAT=$SPLAT
report ambiguous; A_ST=$STICKY A_DISK=$DISK A_PAR=$PAR A_SPLAT=$SPLAT
report quiet;     Q_ST=$STICKY Q_DISK=$DISK Q_PAR=$PAR Q_SPLAT=$SPLAT Q_TORN=$TORN
report inflight;  I_ST=$STICKY I_DISK=$DISK I_PAR=$PAR I_SPLAT=$SPLAT I_TORN=$TORN
report remount;   R_ST=$STICKY R_DISK=$DISK R_PAR=$PAR R_SPLAT=$SPLAT R_TORN=$TORN R_GU=$GU
report remountctl; RC_ST=$STICKY RC_DISK=$DISK RC_PAR=$PAR RC_SPLAT=$SPLAT RC_TORN=$TORN RC_GU=$GU
echo

case "$F_ST$F_DISK$F_PAR$C_ST$C_DISK$A_ST$A_PAR$Q_ST$Q_DISK$Q_PAR$I_ST$I_DISK$I_PAR$R_ST$R_DISK$R_PAR$RC_ST$RC_PAR" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
if [ $((F_SPLAT + C_SPLAT + A_SPLAT + Q_SPLAT + I_SPLAT + R_SPLAT + RC_SPLAT)) -ne 0 ]; then
	echo "RESULT: FAIL -- kernel splat"; exit 1
fi
# An arm whose prep had no write acknowledged over the failing device has no
# record to repair or keep: clean for nothing.
if [ -n "$NOACK" ]; then
	echo "RESULT: INCONCLUSIVE -- no overwrite was acknowledged in the prep of:$NOACK"; exit 2
fi
if [ "$C_ST" -eq 0 ] || [ "$C_DISK" -eq 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control left nothing stale or unrecorded,"
	echo "        so a clean fixed arm proves nothing"
	exit 2
fi
if [ "$F_ST" -ne 0 ]; then
	echo "RESULT: FAIL -- the recovery kept $F_ST record(s) it could have repaired"
	[ "${F_TORN:-0}" -gt 0 ] &&
		echo "        ($F_TORN full stripe(s) taken for possibly torn:" \
		     "see the inflight arm)"
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
if [ "$Q_ST" -ne 0 ] || [ "$Q_DISK" -ne 0 ] || [ "$Q_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- with no repair before the unmount the recovery kept $Q_ST"
	echo "        record(s) ($Q_TORN full stripe(s) taken for possibly torn), $Q_DISK"
	echo "        block(s) still stale on disk, $Q_PAR lost from the parity"
	exit 1
fi
if [ "$I_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- with $KNOB=1, $I_PAR block(s) were lost from the parity"
	exit 1
fi
if [ "$I_ST" -eq 0 ] || [ "${I_TORN:-0}" -eq 0 ]; then
	echo "RESULT: INCONCLUSIVE -- with $KNOB=1 the recovery took no stripe for"
	echo "        possibly torn ($I_ST kept), so the quiet arm's repair does not show"
	echo "        what the log saying the writes finished is for (not a"
	echo "        CONFIG_BTRFS_DEBUG kernel?)"
	exit 2
fi
for a in remount remountctl; do
	grep -q "remounted read-only" $T/umltest/recover-scrub-$a/log.nocow_persist_prep.* \
		2>/dev/null && continue
	echo "RESULT: INCONCLUSIVE -- the $a arm's prep could not remount read-only"; exit 2
done
# Without a retry that gave up there is nothing listed in flight for the
# remount to drop: the remount arm is the quiet arm again.
if [ "${R_GU:-0}" -lt 1 ] || [ "${RC_GU:-0}" -lt 1 ]; then
	echo "RESULT: INCONCLUSIVE -- no repair retried and gave up before the heal" \
	     "(remount ${R_GU:-?}, remountctl ${RC_GU:-?})"
	exit 2
fi
if [ "$R_ST" -ne 0 ] || [ "$R_DISK" -ne 0 ] || [ "$R_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- remounted read-only before the unmount, the recovery kept $R_ST"
	echo "        record(s) ($R_TORN full stripe(s) taken for possibly torn), $R_DISK"
	echo "        block(s) still stale on disk, $R_PAR lost from the parity"
	exit 1
fi
if [ "$RC_PAR" -ne 0 ]; then
	echo "RESULT: FAIL -- with raid56_wf_remount_ro_keeps_inflight=1, $RC_PAR block(s) were lost"
	echo "        from the parity"
	exit 1
fi
if [ "$RC_ST" -eq 0 ] || [ "${RC_TORN:-0}" -eq 0 ]; then
	echo "RESULT: INCONCLUSIVE -- with raid56_wf_remount_ro_keeps_inflight=1 the recovery"
	echo "        took no stripe for possibly torn ($RC_ST kept): no repair retry was left"
	echo "        listed in flight for the remount to drop"
	exit 2
fi
echo "RESULT: PASS -- recovery repaired what the record proves (control left"
echo "        $C_DISK stale, $C_ST recorded), declined what it cannot decide"
echo "        ($A_ST kept, nothing lost); with no repair before the unmount it"
echo "        repaired all again, but with the finished writes left in flight"
echo "        ($KNOB=1) it took $I_TORN stripe(s) for possibly torn and kept $I_ST"
echo "        record(s), $I_DISK block(s) stale; remounted read-only after repairs retried"
echo "        and failed, it repaired all again, but with the remount leaving the retries"
echo "        listed in flight it took $RC_TORN stripe(s) for possibly torn and kept $RC_ST"
