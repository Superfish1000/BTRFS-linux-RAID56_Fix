#!/bin/bash
# Does an ordinary read return a block the write-intent record names as stale?
#
#   stale_read.sh <kernel> [ndev] [fail-device]
#
# A device failed writes under nodatacow overwrites, so some acknowledged
# blocks are on the parity only; the platters still hold the old content, and
# the record names the column.  Then a READ-ONLY mount with every device
# present: no recovery runs, nothing is missing, so every block is read
# straight off its device.  For a block with no checksum nothing used to
# object, and the old content came back as the file's -- although the record
# saying so was on the disk the whole time.
#
# Two arms, each a fresh prep (triggered repair off, so the stale columns
# survive) and a ro mount reading every acknowledged block:
#   direct      0 wrong
#   direct-ctl  btrfs.raid56_read_ignores_stale=1: the old behaviour
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: stale_read.sh <kernel> [ndev] [fail-device]}
NDEV=${2:-4}
FAIL=${3:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-sread.sh.$$ && mv -f $T/umltest/init-sread.sh.$$ $T/umltest/init-sread.sh	# atomic: a guest may be reading it
ulimit -c 0

arm() {	# name knob
	local tag=stale-read-$1 knob="${2:-}" d ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/results.$tag $T/umltest/nocow.bad.*.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	for d in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$d=$D/disk$d.img"; done
	for m in "nocow_persist_prep /dev/mapper/d0 NOPERSIST=0 btrfs.raid56_no_repair_on_fault=1" \
		 "nocow_probe /dev/ubda PROBE=direct $knob"; do
		set -- $m
		local mode=$1 mnt=$2; shift 2
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-sread.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
			MNTDEV=$mnt NDEV=$NDEV FAIL=$FAIL "$@" > $D/log.$mode 2>&1
		echo "boot $mode rc=$?" >> $T/umltest/results.$tag
	done
	cat $T/umltest/nocow.bad.direct.$tag 2>/dev/null || echo "?"
}

fix=$(arm direct)
ctl=$(arm direct-ctl btrfs.raid56_read_ignores_stale=1)
spl=$(grep -l KERNEL_SPLAT $T/umltest/stale-read-*/log.* 2>/dev/null | wc -l)
echo "  wrong blocks read with every device present, ro: fixed $fix, control $ctl, splats $spl"
case "$fix$ctl" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
[ "$ctl" -gt 0 ] || { echo "RESULT: INCONCLUSIVE -- the control read nothing stale, so a clean fix proves nothing"; exit 2; }
[ "$fix" -eq 0 ] || { echo "RESULT: FAIL -- $fix block(s) still read back stale"; exit 1; }
echo "RESULT: PASS -- every acknowledged block reads back correctly; without"
echo "        consulting the record, $ctl came back as the old content, silently"
