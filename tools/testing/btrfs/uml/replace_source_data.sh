#!/bin/bash
# Does a RAID5 device replace keep what the old device holds: data without a
# checksum that it returns, and a column whose copy the replace did not finish?
#
#   replace_source_data.sh <kernel>
#
# See replace_source_data and replace_abort_marks in init-final3.sh.
#   rot       fixed    the parity under two rows of the old device's column
#                      has rotted, unrecorded; the replace copies the rows and
#                      both read back as 'A'
#             control  raid56_wf_replace_rebuilds_unchecked=1: the replace
#                      rebuilds them from the rotten parity, and they read
#                      back wrong without an error
#   rotread   fixed    as rot, and another row of the column fails to read, so
#                      the scrub's read of the column fails as a whole: the
#                      replace asks the old device again row by row and still
#                      copies both
#             control  the rebuild again, both rows wrong
#   abort     fixed    a replace that cannot copy one sector and then aborts
#                      drops the stale mark it made: the old device's good row
#                      next to it reads 'A', and no stale mark is left
#             control  raid56_wf_replace_keeps_marks=1: the mark stays on the
#                      old device's column and that row reads EIO
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_source_data.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-rsd.sh.$$ && mv -f $T/umltest/init-rsd.sh.$$ $T/umltest/init-rsd.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ && mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# mode name control unread
	local tag=rsd-$2 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rsd.$tag $T/umltest/ram.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rsd.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$1 OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$3 UNREAD=$4 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm replace_source_data rot-fixed 0 0
arm replace_source_data rot-control 1 0
arm replace_source_data rotread-fixed 0 1
arm replace_source_data rotread-control 1 1
arm replace_abort_marks abort-fixed 0 0
arm replace_abort_marks abort-control 1 0
for a in rot-fixed rot-control rotread-fixed rotread-control abort-fixed abort-control; do
	grep -ah "replace rc\|replace status\|layout:\|unreadable:\|rotten\|before the\|after\|health:\|healed\|RSD \|RAM \|control:\|could neither copy\|did not finish\|rebuilt from the parity\|KERNEL_SPLAT\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|ROT_FAIL\|DM_RELOAD\|KNOB_FAIL" \
		$T/umltest/rsd-$a/log | head -40 | sed "s/^/  [$a] /"
done
# rsd: rrc pre-row0-A n0 a0 n3 a3 n8 a8 md5ok
read -r rf_rc rf_pre rf_n0 rf_a0 rf_n3 rf_a3 rf_n8 rf_a8 rf_md5 < $T/umltest/rsd.rsd-rot-fixed 2>/dev/null || rf_rc=?
read -r rc_rc rc_pre rc_n0 rc_a0 rc_n3 rc_a3 rc_n8 rc_a8 rc_md5 < $T/umltest/rsd.rsd-rot-control 2>/dev/null || rc_rc=?
read -r uf_rc uf_pre uf_n0 uf_a0 uf_n3 uf_a3 uf_n8 uf_a8 uf_md5 < $T/umltest/rsd.rsd-rotread-fixed 2>/dev/null || uf_rc=?
read -r uc_rc uc_pre uc_n0 uc_a0 uc_n3 uc_a3 uc_n8 uc_a8 uc_md5 < $T/umltest/rsd.rsd-rotread-control 2>/dev/null || uc_rc=?
# ram: rrc inuse pre5-A n5 a5 n0 marks uncopyable md5ok
read -r af_rc af_in af_pre af_n5 af_a5 af_n0 af_marks af_unc af_md5 < $T/umltest/ram.rsd-abort-fixed 2>/dev/null || af_rc=?
read -r ac_rc ac_in ac_pre ac_n5 ac_a5 ac_n0 ac_marks ac_unc ac_md5 < $T/umltest/ram.rsd-abort-control 2>/dev/null || ac_rc=?
case "$rf_rc$rc_rc$uf_rc$uc_rc$af_rc$ac_rc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rsd-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
echo "  rot:     fixed rows 0/3 $rf_a0/$rf_a3 of 4096 'A' md5ok=$rf_md5 (replace rc $rf_rc), control $rc_a0/$rc_a3 'A' of $rc_n0/$rc_n3 read (replace rc $rc_rc)"
echo "  rotread: fixed rows 0/3/8 $uf_a0/$uf_a3/$uf_a8 'A' md5ok=$uf_md5 (replace rc $uf_rc), control $uc_a0/$uc_a3 'A' of $uc_n0/$uc_n3 read (replace rc $uc_rc)"
echo "  abort:   fixed replace rc $af_rc, row 5 $af_n5 bytes ($af_a5 'A'), stale_marks $af_marks, md5ok=$af_md5;" \
     "control replace rc $ac_rc, row 5 $ac_n5 bytes, stale_marks $ac_marks"
# The controls must show the defect, or the case was not produced.
for c in "rot $rc_rc $rc_pre $rc_n0 $rc_a0 $rc_n3 $rc_a3" "rotread $uc_rc $uc_pre $uc_n0 $uc_a0 $uc_n3 $uc_a3"; do
	set -- $c
	if [ "$2" != 0 ] || [ "$3" != 4096 ] || [ "$4" != 4096 ] || [ "$6" != 4096 ] ||
	   { [ "$5" = 4096 ] && [ "$7" = 4096 ]; }; then
		echo "RESULT: INCONCLUSIVE -- the $1 control did not read a rotten row back wrong without an error; the injection did not produce the case"
		exit 2
	fi
done
if [ "$ac_rc" = 0 ] || [ "$ac_in" != 1 ] || [ "$ac_pre" != 4096 ] || [ "$ac_n5" != 0 ] ||
   ! [ "${ac_marks:-0}" -ge 1 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- the abort control did not abort with the mark left on the old device's column (row 5 must fail); the case was not produced"
	exit 2
fi
for f in "rot $rf_rc $rf_n0 $rf_a0 $rf_n3 $rf_a3 $rf_md5" "rotread $uf_rc $uf_n0 $uf_a0 $uf_n3 $uf_a3 $uf_md5"; do
	set -- $f
	[ "$2" = 0 ] || { echo "RESULT: FAIL -- the $1 replace did not finish"; exit 1; }
	if [ "$3" != 4096 ] || [ "$4" != 4096 ] || [ "$5" != 4096 ] || [ "$6" != 4096 ] || [ "$7" != 1 ]; then
		echo "RESULT: FAIL -- $1: the new device does not hold what the old one returned"; exit 1
	fi
done
[ "$uf_a8" = 4096 ] || { echo "RESULT: FAIL -- rotread: the row the old device could not return was not rebuilt"; exit 1; }
[ "$af_rc" != 0 ] && [ "$af_in" = 1 ] || { echo "RESULT: FAIL -- the abort case did not abort (rc $af_rc)"; exit 1; }
[ "${af_unc:-0}" -ge 1 ] 2>/dev/null || { echo "RESULT: FAIL -- the aborted replace did not count the sector it could not copy"; exit 1; }
[ "$af_n5" = 4096 ] && [ "$af_a5" = 4096 ] || { echo "RESULT: FAIL -- after the abort the old device's good row reads $af_n5 bytes ($af_a5 'A')"; exit 1; }
[ "$af_marks" = 0 ] || { echo "RESULT: FAIL -- the aborted replace left $af_marks stale mark(s)"; exit 1; }
[ "$af_md5" = 1 ] || { echo "RESULT: FAIL -- the file does not read back once the sibling is healed"; exit 1; }
echo "RESULT: PASS -- the replace copies data without a checksum from the old device (control: rebuilt from rotten parity, wrong);"
echo "        an aborted replace drops its stale mark (control: the old device's good row reads EIO)"
