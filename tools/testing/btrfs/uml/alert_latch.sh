#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does an alert that waits for an acknowledgment survive a crash and a
# remount, and does the acknowledgment?
#
#   alert_latch.sh <kernel>
#
# See alert_latch in init-final3.sh.  RAID5 data, RAID1C3 metadata, four
# devices.  The log slots of three of them fail writes, and a write fails
# before it reaches a disk: log_write_failed, which stays until 'ack'.  They
# are healed, another write goes through, and the machine stops without
# syncing.  Then the array is mounted, unmounted and mounted again, the alert
# acknowledged, and the machine stops again; one more mount.
#   fixed    the log carries the alert (BTRFS_WIB_LATCH_OFFSET): raid56_health
#            shows it unacknowledged after the crash and after the remount,
#            the state failing, and not after the ack and the crash after it
#   control  raid56_wf_latch_volatile=1: the alert is in memory only, and the
#            mount after the crash shows nothing unacknowledged
#
#   alert_latch.sh <kernel> narrow
#
# The alert raised the same way, then 165 overwrites in regions of their own
# (rows whose full stripe lies in one region, raid56_rows.py),
# finished one after the other and not committed, and the crash: the log's
# block lists each (the union with the last block), and 165 is what the narrow
# layout holds -- its last entry covers the alert (BTRFS_WIB_LATCH_OFFSET).
#   fixed    no block lists more than 164 (BTRFS_WIB_NARROW_WRITE_MAX): the
#            newest carries the alert, and the mount after the crash shows it
#   control  raid56_wf_latch_full_narrow=1: the newest block lists 165 and no
#            alert, and the mount shows none
#   upgrade  that knob in the boot that writes, not in the mount after the
#            crash: the newest block has no room, and the mount takes the
#            alert from the device's other slot
# The host reads the log slots of the images between the boots
# (raid56_wib_dump.py).
#
#   alert_latch.sh <kernel> disabled
#
# The alert raised the same way, then the feature cleared: the log's last
# block carries the alert, and it is not written again.  Mounted with
# -o noraid56_write_intent, the alert is back; it is acknowledged, and the
# machine stops.  Mounted so again:
#   fixed    nothing stands: the ack went to the log, the newest block written
#            again with it (wib_stamp_latch_locked())
#   control  raid56_wf_latch_needs_log=1: nothing wrote the ack, and the alert
#            is back -- at every mount, for good
#
#   alert_latch.sh <kernel> unmount
#
# One boot.  A write goes through; then the log slots of three devices of four
# fail every IO, and the filesystem is unmounted: the unmount's transaction
# commit cannot write the log and fails (log_commit_failed, read-only), and the
# alert reaches the fourth device.  Healed and mounted read-only, the alert is
# acknowledged; then unmounted, mounted read-write, acknowledged there, and
# mounted again.  In both arms log_commit_failed stands on the read-only
# mount, again on the read-write mount (an ack on a read-only mount cannot
# reach the log), and is gone after the ack there.
#   fixed    the read-only ack says it lasts that mount only
#   control  raid56_wf_latch_unmount_drops=1: it says nothing
# The same knob restores an unmount that discards the latch work queued a
# moment before and writes nothing an error latched once the alerts stopped;
# no arm reaches either (README.md, Known gaps).
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: alert_latch.sh <kernel> [narrow|disabled|unmount]}
PLAN=${2:-latch}
case $PLAN in latch|narrow|disabled|unmount) ;; *) echo "unknown plan $PLAN"; exit 2;; esac
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-al.sh.$$ &&
	mv -f $T/umltest/init-al.sh.$$ $T/umltest/init-al.sh
ulimit -c 0
boot() {	# phase opts control
	local ubds="" d
	for d in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$d=$D/disk$d.img"; done
	timeout 900 $KERNEL mem=512M rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-al.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=alert_latch OPTS=$2 PROFILE=raid5:raid1c3 TAG=$tag \
		NDEV=$NDEV CONTROL=$3 MNTDEV=/dev/ubda PHASE=$1 PLAN=$PLAN \
		< /dev/null > $D/log.$1 2>&1
	echo "boot $1 rc=$?" >> $D/log.boots
}
arm() {	# name control
	tag=al-$1
	D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/al.$tag.* $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 256M $D/disk$i.img; done
	# commit=600: no transaction commit meets the failing log slots.
	boot raise rw,commit=600 $2
	boot check rw $2
	boot final rw $2
	rm -f $D/disk*.img
}
if [ $PLAN = unmount ]; then
	for a in fixed:0 control:1; do
		tag=al-u${a%%:*}
		D=$T/umltest/$tag
		rm -rf $D; mkdir -p $D
		rm -f $T/umltest/al.$tag.* $T/umltest/results.$tag
		for i in $(seq 0 $((NDEV-1))); do truncate -s 256M $D/disk$i.img; done
		boot uraise rw,commit=600 ${a##*:}
		rm -f $D/disk*.img
		grep -ahE "control:|AL |commit FAILED|failing the transaction|KERNEL_SPLAT|WATCHDOG|_FAIL" \
			$D/log.* | cut -c1-300 | sed "s/^/  [${a%%:*}] /"
	done
	res() { cat $T/umltest/al.al-u$1.uraise 2>/dev/null || echo "? ? ? ? ?"; }
	read -r f_w f_u1 f_msg f_u2 f_u3 <<<"$(res fixed)"
	read -r c_w c_u1 c_msg c_u2 c_u3 <<<"$(res control)"
	echo "  unacknowledged after the failed unmount, read-only / read-write after a read-only"
	echo "  ack / after an ack there (the read-only ack said it lasts that mount only):"
	echo "    fixed   $f_u1 / $f_u2 / $f_u3 ($f_msg)"
	echo "    control $c_u1 / $c_u2 / $c_u3 ($c_msg)"
	case "$f_w$c_w" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/al-u*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/al-u*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/al-ucontrol/log.* &&
		{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
	grep -aq "commit FAILED" $T/umltest/al-ufixed/log.* $T/umltest/al-ucontrol/log.* ||
		{ echo "RESULT: INCONCLUSIVE -- the unmount's commit did not fail"; exit 2; }
	has() { case ",$1," in *,log_commit_failed,*) return 0;; esac; return 1; }
	for v in "$f_w:$f_u1:$f_u2:$f_u3" "$c_w:$c_u1:$c_u2:$c_u3"; do
		case "$v" in ok:*log_commit_failed*:*log_commit_failed*:none) ;; *)
			echo "RESULT: INCONCLUSIVE -- the alert did not stand after the unmount and the"
			echo "        read-only ack, or did after the read-write one ($v)"
			exit 2;;
		esac
	done
	if [ "$c_msg" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control's read-only ack said something ($c_msg)"
		exit 2
	fi
	if [ "$f_msg" != 1 ]; then
		echo "RESULT: FAIL -- the ack on the read-only mount did not say it lasts that mount" \
		     "only, and the alert came back ($f_u2)"
		exit 1
	fi
	echo "RESULT: PASS -- the ack on the read-only mount said it lasts that mount only (the"
	echo "        alert stood again on the read-write mount, and went with the ack there);"
	echo "        control: the read-only ack said nothing and the alert came back all the same"
	exit 0
fi
if [ $PLAN = disabled ]; then
	darm() {	# name control
		tag=al-d$1
		D=$T/umltest/$tag
		rm -rf $D; mkdir -p $D
		rm -f $T/umltest/al.$tag.* $T/umltest/results.$tag
		for i in $(seq 0 $((NDEV-1))); do truncate -s 256M $D/disk$i.img; done
		boot draise rw,commit=600 $2
		boot dcheck rw $2
		rm -f $D/disk*.img
	}
	darm fixed 0
	darm control 1
	for a in fixed control; do
		grep -ahE "control:|AL |alerts an earlier mount|acknowledged|disabled|KERNEL_SPLAT|WATCHDOG|_FAIL" \
			$T/umltest/al-d$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
	done
	res() { cat $T/umltest/al.al-d$1.$2 2>/dev/null || echo "? ? ? ? ? ?"; }
	read -r f_w1 f_en0 f_u0 f_en1 f_u1 f_u2 <<<"$(res fixed draise)"
	read -r c_w1 c_en0 c_u0 c_en1 c_u1 c_u2 <<<"$(res control draise)"
	read -r f_en3 f_u3 <<<"$(res fixed dcheck)"
	read -r c_en3 c_u3 <<<"$(res control dcheck)"
	echo "  unacknowledged, disabled / mounted again / after the ack / after the crash" \
	     "(log enabled then):"
	echo "    fixed   $f_u0 / $f_u1 / $f_u2 / $f_u3 ($f_en0 $f_en1 $f_en3)"
	echo "    control $c_u0 / $c_u1 / $c_u2 / $c_u3 ($c_en0 $c_en1 $c_en3)"
	case "$f_w1$c_w1$f_u3$c_u3" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/al-d*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/al-d*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/al-dcontrol/log.* &&
		{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
	has() { case ",$1," in *,log_write_failed,*) return 0;; esac; return 1; }
	for v in "$f_w1:$f_en0:$f_en1:$f_en3" "$c_w1:$c_en0:$c_en1:$c_en3"; do
		[ "$v" = eio:0:0:0 ] || {
			echo "RESULT: INCONCLUSIVE -- the write did not fail, or the log was enabled ($v)"
			exit 2
		}
	done
	if ! has "$f_u1" || ! has "$c_u1" || [ "$f_u2" != none ] || [ "$c_u2" != none ]; then
		echo "RESULT: INCONCLUSIVE -- the disabled log's alert was not back, or the ack" \
		     "did not clear it (fixed $f_u1/$f_u2, control $c_u1/$c_u2)"
		exit 2
	fi
	if ! has "$c_u3"; then
		echo "RESULT: INCONCLUSIVE -- the control's ack stuck ($c_u3): it did not reproduce"
		exit 2
	fi
	if [ "$f_u3" != none ]; then
		echo "RESULT: FAIL -- with the log disabled, the acknowledged alert came back ($f_u3)"
		exit 1
	fi
	echo "RESULT: PASS -- with the log disabled, the alert its last block carries was"
	echo "        acknowledged for good; control: it came back after the crash"
	exit 0
fi
if [ $PLAN = narrow ]; then
	cp $HERE/../raid56_wib_dump.py $T/umltest/raid56_wib_dump.py.$$ &&
		mv -f $T/umltest/raid56_wib_dump.py.$$ $T/umltest/raid56_wib_dump.py
	cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
		mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
	narm() {	# name control-while-writing control-at-mount
		tag=al-n$1
		D=$T/umltest/$tag
		rm -rf $D; mkdir -p $D
		rm -f $T/umltest/al.$tag.* $T/umltest/results.$tag
		for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
		boot nraise rw,commit=600 $2
		python3 $T/umltest/raid56_wib_dump.py $D/disk*.img > $D/slots 2>&1
		boot check rw $3
		rm -f $D/disk*.img
	}
	narm fixed 0 0
	narm control 1 1
	narm upgrade 1 0
	# The newest valid block of each device: its entries and the alerts.
	newest() {
		awk '/ slot [01]: seq / { ok = ($0 ~ /csum OK/); seq = $5 + 0; nr = $7; next }
		     /unacknowledged:/ && ok { sub(/.*unacknowledged: /, "")
			 if (seq > best) { best = seq; line = nr " " $0 } }
		     END { print line }' $T/umltest/al-n$1/slots
	}
	for a in fixed control upgrade; do
		grep -ahE "control:|AL |alerts an earlier mount|KERNEL_SPLAT|WATCHDOG|_FAIL" \
			$T/umltest/al-n$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
		sed -n 's/^\(.*img slot [01]: seq [0-9]* entries [0-9]*\).*/\1/p;s/^ *unacknowledged:/    unacknowledged:/p' \
			$T/umltest/al-n$a/slots | sed "s/^/  [$a] /"
	done
	res() { cat $T/umltest/al.al-n$1.$2 2>/dev/null || echo "? ? ? ? ?"; }
	bad=""; incon=""
	for a in fixed control upgrade; do
		read -r w1 ok regs commits u0 <<<"$(res $a nraise)"
		read -r u1 _ <<<"$(res $a check)"
		read -r nr latch <<<"$(newest $a)"
		echo "  $a: first write $w1, $ok overwrites in $regs regions, commit $commits," \
		     "raised $u0; newest block $nr regions, carries: $latch; after the crash: $u1"
		case "$w1$ok$u1" in *'?'*) incon="$incon $a:no-report";; esac
		case ",$u0," in *,log_write_failed,*) ;; *) incon="$incon $a:not-raised";; esac
		[ "$w1" = eio ] && [ "$ok" = 165 ] && [ "$regs" = 165 ] && [ "$commits" = 0 ] ||
			incon="$incon $a:setup"
		eval "${a}_u1=\$u1 ${a}_nr=\$nr ${a}_latch=\"\$latch\""
	done
	grep -lq KERNEL_SPLAT $T/umltest/al-n*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/al-n*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/al-n*/log.* &&
		{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
	[ -z "$incon" ] || { echo "RESULT: INCONCLUSIVE --$incon"; exit 2; }
	has() { case ",$1," in *,log_write_failed,*) return 0;; esac; return 1; }
	if [ "$control_nr" != 165 ] || has "$control_u1"; then
		echo "RESULT: INCONCLUSIVE -- the control's newest block listed $control_nr regions" \
		     "and the mount showed $control_u1: it did not reproduce the lost alert"
		exit 2
	fi
	if [ "$upgrade_nr" != 165 ]; then
		echo "RESULT: INCONCLUSIVE -- upgrade: the newest block listed $upgrade_nr regions"
		exit 2
	fi
	if [ "${fixed_nr:-999}" -gt 164 ] || [ "$fixed_latch" != log_write_failed ] ||
	   ! has "$fixed_u1"; then
		echo "RESULT: FAIL -- fixed: the newest block listed $fixed_nr regions carrying" \
		     "'$fixed_latch', and the mount after the crash showed $fixed_u1"
		exit 1
	fi
	if ! has "$upgrade_u1"; then
		echo "RESULT: FAIL -- upgrade: the alert in the other slot was not taken ($upgrade_u1)"
		exit 1
	fi
	echo "RESULT: PASS -- no block listed more than 164 regions and the alert survived the"
	echo "        crash; from a block of 165 it was taken from the other slot; control:"
	echo "        a block of 165 carried nothing and the alert was gone"
	exit 0
fi
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ahE "control:|AL |alerts an earlier mount|KERNEL_SPLAT|WATCHDOG|_FAIL" \
		$T/umltest/al-$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
done
res() { cat $T/umltest/al.al-$1.$2 2>/dev/null || echo "? ? ? ?"; }
read -r f_w1 f_w2 f_u0 <<<"$(res fixed raise)"
read -r c_w1 c_w2 c_u0 <<<"$(res control raise)"
read -r f_u1 f_u2 f_u3 f_st <<<"$(res fixed check)"
read -r c_u1 c_u2 c_u3 c_st <<<"$(res control check)"
read -r f_u4 <<<"$(res fixed final)"
read -r c_u4 <<<"$(res control final)"
echo "  unacknowledged, raised / after the crash / after a remount / after the ack /" \
     "after the next crash:"
echo "    fixed   $f_u0 / $f_u1 / $f_u2 / $f_u3 / $f_u4 (state $f_st)"
echo "    control $c_u0 / $c_u1 / $c_u2 / $c_u3 / $c_u4 (state $c_st)"
case "$f_w1$c_w1$f_u1$c_u1$f_u4$c_u4" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
grep -lq KERNEL_SPLAT $T/umltest/al-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/al-*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/al-control/log.* &&
	{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
has() { case ",$1," in *,log_write_failed,*) return 0;; esac; return 1; }
if [ "$f_w1" != eio ] || [ "$c_w1" != eio ] || ! has "$f_u0" || ! has "$c_u0"; then
	echo "RESULT: INCONCLUSIVE -- the write did not fail with log_write_failed"
	echo "        (fixed $f_w1 $f_u0, control $c_w1 $c_u0)"
	exit 2
fi
if [ "$c_u1" != none ]; then
	echo "RESULT: INCONCLUSIVE -- the control kept the alert across the crash ($c_u1)"
	exit 2
fi
if ! has "$f_u1" || ! has "$f_u2" || [ "$f_st" != failing ]; then
	echo "RESULT: FAIL -- the alert did not survive the crash ($f_u1) or the remount ($f_u2)," \
	     "state $f_st"
	exit 1
fi
if [ "$f_u3" != none ] || [ "$f_u4" != none ]; then
	echo "RESULT: FAIL -- the acknowledged alert came back ($f_u3, after the crash $f_u4)"
	exit 1
fi
grep -aq "alerts an earlier mount raised that nobody acknowledged: log_write_failed" \
	$T/umltest/al-fixed/log.check ||
	{ echo "RESULT: FAIL -- the mount did not say which alerts it took back"; exit 1; }
echo "RESULT: PASS -- log_write_failed stood after a crash and a remount until acknowledged,"
echo "        and the acknowledgment survived the next crash; control: forgotten at the crash"
