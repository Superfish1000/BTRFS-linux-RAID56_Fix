#!/bin/bash
# Does a RAID5 device replace give the new device whole data columns, and
# what happens to a sector it can neither copy nor rebuild?
#
#   replace_whole_column.sh <kernel>
#
# See replace_whole_column and replace_uncopyable in init-final3.sh.
#   column    fixed    every file reads back right with either other device
#                      left out
#             control  raid56_wf_replace_extents_only=1: the free columns never
#                      reach the target, and files rebuilt against them come
#                      back wrong without an error
#   lost      fixed    the replace counts the sector it could not copy; its
#                      block reads EIO while the sibling's sector fails, and
#                      'A' once that is healed
#             control  the replace counts nothing and the block reads back as
#                      the target's zeros, both times
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_whole_column.sh <kernel>}
NDEV=4
NF=192
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-rwc.sh.$$ && mv -f $T/umltest/init-rwc.sh.$$ $T/umltest/init-rwc.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# mode name control
	local tag=rwc-$2 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rwc.$tag $T/umltest/ruc.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rwc.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$1 OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV NF=$NF CONTROL=$3 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm replace_whole_column column-fixed 0
arm replace_whole_column column-control 1
arm replace_uncopyable lost-fixed 0
arm replace_uncopyable lost-control 1
for a in column-fixed column-control lost-fixed lost-control; do
	grep -ah "replace rc\|replace status\|without \|WRONG\|layout:\|unreadable:\|health:\|with the sibling\|sibling healed\|RWC \|RUC \|control:\|could neither copy\|can neither copy\|KERNEL_SPLAT\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|DM_RELOAD\|KNOB_FAIL" \
		$T/umltest/rwc-$a/log | head -40 | sed "s/^/  [$a] /"
done
read -r cf_rc cf_unc cf_ok cf_wrong cf_eio < $T/umltest/rwc.rwc-column-fixed 2>/dev/null || cf_rc=?
read -r cc_rc cc_unc cc_ok cc_wrong cc_eio < $T/umltest/rwc.rwc-column-control 2>/dev/null || cc_rc=?
read -r lf_rc lf_unc lf_n1 lf_a1 lf_n2 lf_a2 lf_md5 lf_cnt < $T/umltest/ruc.rwc-lost-fixed 2>/dev/null || lf_rc=?
read -r lc_rc lc_unc lc_n1 lc_a1 lc_n2 lc_a2 lc_md5 lc_cnt < $T/umltest/ruc.rwc-lost-control 2>/dev/null || lc_rc=?
case "$cf_rc$cc_rc$lf_rc$lc_rc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rwc-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
echo "  column: fixed ok=$cf_ok wrong=$cf_wrong eio=$cf_eio (replace rc $cf_rc), control ok=$cc_ok wrong=$cc_wrong eio=$cc_eio (replace rc $cc_rc)"
echo "  lost:   fixed uncorr=$lf_unc read $lf_n1 bytes failing / $lf_a2 of 4096 'A' healed, md5 $lf_md5, alerts $lf_cnt;" \
     "control uncorr=$lc_unc read $lc_n1 bytes ($lc_a1 'A') failing / $lc_a2 'A' healed"
if [ "$cc_rc" != 0 ] || [ "$cc_wrong" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's replace left no file rebuilt against a missing column; the layout did not produce the case"
	exit 2
fi
if [ "$lc_rc" != 0 ] || [ "$lc_n1" != 4096 ] || [ "$lc_a1" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not read back the uncopied sector as something else; the injection did not produce the case"
	exit 2
fi
[ "$cf_rc" = 0 ] || { echo "RESULT: FAIL -- the replace of a healthy array failed"; exit 1; }
if [ "$cf_wrong" != 0 ] || [ "$cf_eio" != 0 ] || [ "$cf_ok" != "$NF" ]; then
	echo "RESULT: FAIL -- files rebuilt against the new device do not read back as written"; exit 1
fi
[ "$lf_rc" = 0 ] || { echo "RESULT: FAIL -- the replace with one uncopyable sector did not finish"; exit 1; }
[ "${lf_unc:-0}" -ge 1 ] 2>/dev/null || { echo "RESULT: FAIL -- the replace did not count the sector it could not copy"; exit 1; }
[ "$lf_n1" = 0 ] || { echo "RESULT: FAIL -- the uncopied sector read back ($lf_n1 bytes) instead of failing"; exit 1; }
if [ "$lf_a2" != 4096 ] || [ "$lf_md5" != 1 ]; then
	echo "RESULT: FAIL -- the uncopied sector does not rebuild once its sibling is back"; exit 1
fi
[ "${lf_cnt:-0}" -ge 1 ] 2>/dev/null || { echo "RESULT: FAIL -- no replace_uncopyable alert"; exit 1; }
echo "RESULT: PASS -- whole columns: every file reads back with either device missing (control: $cc_wrong wrong);"
echo "        the uncopyable sector is counted and fails until it can be rebuilt (control: reads back zeros)"
