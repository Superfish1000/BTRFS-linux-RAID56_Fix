#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does the write-intent log get out of an owed readd -- the records a failed
# flush kept, which no block can take back yet -- while writes into new
# regions keep coming?
#
#   readd_admit.sh <kernel> [new|hot|say]
#
# See readd_admit in init-final3.sh.  RAID5 data, RAID1 metadata on four
# devices, a fifth added that holds none of the file's stripes.  The log's last
# block lists 150-odd regions; the fifth device fails its flushes, eight writers
# write into regions of their own the last block does not list, the device is
# healed and they go on.  Scored: the writes started RA_SETTLE_SECS or more
# after the heal ("late").
#   fixed    no such write is recorded while the readd is owed: it waits, the
#            readd is done once the writes holding its room are, and every
#            late write succeeds
#   control  raid56_wf_readd_admits_new=1: they are recorded, the block after
#            the readd never fits, and late writes keep failing
#            (log_write_failed) although the device works again
# hot: the last block lists 160 regions, close to the 165 it holds; 32
# writers, four to a region, start at once, each rewriting a block of a full
# stripe of its own there from one process (thread_pool=32, so that more than
# three are recorded at once), and the eight regions stay busy -- those the
# block after the fill could not list are what the owed readd waits for.  The fifth device fails one flush,
# the one the log needs to take the first of them, and is healed at once;
# scored for 40 s after that, past BTRFS_WIB_READD_WAIT, "late" from 3 s
# after it.
#   fixed    a write into such a region waits as one into a new region does,
#            the readd is done once the writes in flight there are, and
#            every late write succeeds: refusing them stalls nobody
#   control  raid56_wf_readd_admits_busy=1: they are recorded and keep the
#            regions busy -- for as long as more than one write at a time is
#            recorded there.  UML records no more than about four at once
#            (one CPU, and the read-modify-write reads queue on its device
#            threads), so the regions drain within a second either way and
#            the readd does not wait until it gives up: reported, not
#            scored.  The selftest test_readd_busy_region() is the control
#            that shows the wait and the records it loses.
# say: the new arm's run, the kernel log followed to the end, and the lines
# that say an owed readd's wait for room begins ("dropping nothing from it
# until then") and ends ("took back the records a failed flush kept")
# counted: each failed flush begins one, and each ends within milliseconds.
#   fixed    one wait said per minute (the run is shorter), with how many
#            went unsaid since the last; an end said only for a wait that
#            was, so no more ends than waits
#   control  raid56_wf_readd_says_each=1: a pair for every wait
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: readd_admit.sh <kernel> [new|hot|say]}
ARM=${2:-new}
case $ARM in new|hot|say) ;; *) echo "unknown arm $ARM"; exit 2;; esac
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-ra.sh.$$ && mv -f $T/umltest/init-ra.sh.$$ $T/umltest/init-ra.sh
ulimit -c 0
arm() {	# name control [VAR=value...]
	local tag=ra-$1 ubds="" control=$2
	local D=$T/umltest/$tag
	shift 2
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/ra.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	# commit=600: no transaction commit, whose barrier would drop what the
	# last block lists before the device fails a flush.
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-ra.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=readd_admit OPTS=${OPTS:-rw,commit=600} PROFILE=raid5:raid1 \
		TAG=$tag NDEV=$NDEV CONTROL=$control "$@" < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
if [ $ARM = hot ]; then
	HOT="RA_HOT=1 RA_LAST=160 RA_WRITERS=32 RA_SHARE=4 RA_PER=1 RA_SETTLE_SECS=3 RA_HEAL_SECS=40"
	# More RMW workers than the three one CPU gets, or no more than three
	# writes are ever recorded at once (rmw_workers).
	OPTS=rw,commit=600,thread_pool=32
	# shellcheck disable=SC2086
	arm hot 0 $HOT
	# shellcheck disable=SC2086
	arm hotctl 2 $HOT
	for a in hot hotctl; do
		grep -ah "control:\|RA \|RA_\|fails every flush\|healed\|not waiting\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|DEVADD_FAIL\|FALLOCATE_FAIL\|DM_RELOAD\|KNOB_FAIL" \
			$T/umltest/ra-$a/log | sed "s/^/  [$a] /"
	done
	read -r ff_ok ff_eio fh_ok fh_eio fl_ok fl_eio f_fl f_commit f_lw f_rd f_gu \
		< $T/umltest/ra.ra-hot 2>/dev/null || ff_ok=?
	read -r cf_ok cf_eio ch_ok ch_eio cl_ok cl_eio c_fl c_commit c_lw c_rd c_gu \
		< $T/umltest/ra.ra-hotctl 2>/dev/null || cf_ok=?
	case "$ff_ok$cf_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/ra-hot*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/ra-hot*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/ra-hotctl/log &&
		{ echo "RESULT: INCONCLUSIVE -- raid56_wf_readd_admits_busy is not there (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2; }
	grep -lq RA_FILL_FLUSHED $T/umltest/ra-hot*/log &&
		{ echo "RESULT: INCONCLUSIVE -- the log flushed while its last block was being filled"; exit 2; }
	echo "  writes ok/eio: failing, just healed, late; records dropped after the heal;"
	echo "  times the readd gave up waiting"
	echo "    fixed   $ff_ok/$ff_eio, $fh_ok/$fh_eio, $fl_ok/$fl_eio (log_write_failed $f_lw);" \
	     "$f_rd; $f_gu"
	echo "    control $cf_ok/$cf_eio, $ch_ok/$ch_eio, $cl_ok/$cl_eio (log_write_failed $c_lw);" \
	     "$c_rd; $c_gu"
	if [ "${f_fl:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the fixed arm's log flush did not fail"; exit 2
	fi
	if [ "${fl_ok:-0}" = 0 ] || [ "$fl_eio" != 0 ] || [ "${f_gu:-1}" != 0 ]; then
		echo "RESULT: FAIL -- after the device was healed writes still failed or the readd"
		echo "        waited until it gave up: late ok=$fl_ok eio=$fl_eio, gave up $f_gu"
		exit 1
	fi
	echo "RESULT: PASS -- with writers keeping regions busy, once the device was healed every"
	echo "        late write succeeded ($fl_ok) and the readd never had to give up; recording"
	echo "        writes into those regions (control, not scored) gave up $c_gu time(s),"
	echo "        $cl_eio of $((cl_ok + cl_eio)) late writes failed"
	exit 0
fi
if [ $ARM = say ]; then
	arm say 0 RA_SAY=1
	arm sayctl 3 RA_SAY=1
	for a in say sayctl; do
		grep -ah "control:\|RA \|RA_\|fails every flush\|healed\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|DEVADD_FAIL\|FALLOCATE_FAIL\|DM_RELOAD\|KNOB_FAIL" \
			$T/umltest/ra-$a/log | sed "s/^/  [$a] /"
	done
	read -r f_w f_u f_e f_fl < $T/umltest/ra.say.ra-say 2>/dev/null || f_w=?
	read -r c_w c_u c_e c_fl < $T/umltest/ra.say.ra-sayctl 2>/dev/null || c_w=?
	read -r _ _ _ _ fl_ok fl_eio _ < $T/umltest/ra.ra-say 2>/dev/null || fl_ok=?
	case "$f_w$c_w$fl_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/ra-say*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/ra-say*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/ra-sayctl/log &&
		{ echo "RESULT: INCONCLUSIVE -- raid56_wf_readd_says_each is not there (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2; }
	echo "  waits said (reported unsaid), ends said, log flushes failed:"
	echo "    fixed   $f_w ($f_u), $f_e, $f_fl; late writes ok/eio $fl_ok/$fl_eio"
	echo "    control $c_w ($c_u), $c_e, $c_fl"
	if [ "$c_e" -lt 3 ]; then
		echo "RESULT: INCONCLUSIVE -- the control said $c_e end(s): too few waits to show"
		echo "        what saying each of them costs"
		exit 2
	fi
	if [ "$f_w" -lt 1 ]; then
		echo "RESULT: FAIL -- the fixed arm said no wait at all"; exit 1
	fi
	if [ "$f_w" -gt 1 ] || [ "$f_e" -gt "$f_w" ]; then
		echo "RESULT: FAIL -- the fixed arm said $f_w wait(s) and $f_e end(s) in a run shorter"
		echo "        than a minute"
		exit 1
	fi
	if [ "$fl_ok" = 0 ] || [ "$fl_eio" != 0 ]; then
		echo "RESULT: FAIL -- after the device was healed writes still failed: late" \
		     "ok=$fl_ok eio=$fl_eio"
		exit 1
	fi
	echo "RESULT: PASS -- the waits for room were said once ($f_w, ends $f_e); said each"
	echo "        time, $c_w waits and $c_e ends (control)"
	exit 0
fi
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "control:\|RA \|RA_\|fails every flush\|healed\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|DEVADD_FAIL\|FALLOCATE_FAIL\|DM_RELOAD\|KNOB_FAIL" \
		$T/umltest/ra-$a/log | sed "s/^/  [$a] /"
done
read -r ff_ok ff_eio fh_ok fh_eio fl_ok fl_eio f_fl f_commit f_lw _ < $T/umltest/ra.ra-fixed 2>/dev/null ||
	ff_ok=?
read -r cf_ok cf_eio ch_ok ch_eio cl_ok cl_eio c_fl c_commit c_lw _ < $T/umltest/ra.ra-control 2>/dev/null ||
	cf_ok=?
case "$ff_ok$cf_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/ra-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/ra-*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/ra-control/log &&
	{ echo "RESULT: INCONCLUSIVE -- raid56_wf_readd_admits_new is not there (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2; }
grep -lq RA_FILL_FLUSHED $T/umltest/ra-*/log &&
	{ echo "RESULT: INCONCLUSIVE -- the log flushed while its last block was being filled"; exit 2; }
if [ "${f_fl:-0}" = 0 ] || [ "${c_fl:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no log flush failed (fixed=$f_fl control=$c_fl)"; exit 2
fi
echo "  writes ok/eio: failing, just healed, late"
echo "    fixed   $ff_ok/$ff_eio, $fh_ok/$fh_eio, $fl_ok/$fl_eio (log_write_failed $f_lw)"
echo "    control $cf_ok/$cf_eio, $ch_ok/$ch_eio, $cl_ok/$cl_eio (log_write_failed $c_lw)"
if [ "${cl_eio:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's late writes did not fail, so a clean fixed arm proves nothing"
	exit 2
fi
if [ "${fl_ok:-0}" = 0 ] || [ "$fl_eio" != 0 ]; then
	echo "RESULT: FAIL -- after the device was healed writes still failed: late ok=$fl_ok eio=$fl_eio"
	exit 1
fi
echo "RESULT: PASS -- once the device was healed every late write succeeded ($fl_ok);"
echo "        recording writes into new regions meanwhile, $cl_eio of $((cl_ok + cl_eio)) late writes failed"
