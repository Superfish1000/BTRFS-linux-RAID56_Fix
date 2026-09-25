#!/bin/bash
# open/r6deg: is the branch's repair safe on RAID6 when its own write fails,
# with a device missing?
#
#   r6deg.sh <kernel> [arm...]
#
# RAID6 over five devices (raid1c3 metadata), a nodatacow file.  One 4K block
# of data column 0 (device X) of a full stripe is overwritten in place while X
# fails writes, and Q of that full stripe is either missing (degraded: the Q
# device omitted from the mount) or failing too (healthy).  The overwrite is
# acknowledged -- two faults, within RAID6 -- and the record names X stale and
# Q stale.  The repair queued on the fault runs a second later with X still
# failing, so its phase-A write-back fails and it refuses (rmw_repair_first()).
# rmw_update_stale_parity() then runs with no parity error bits and a full
# dbitmap, and clears the stale mark of a parity that was never written.
# X is healed and the log committed.  Then every device is back and the block
# is read: ro (no recovery; the record read off the disks decides), then rw
# with the mount recovery and a scrub.
#
# Arms (default: all):
#   deg            Q device missing                         (expect: SILENT)
#   deg-single     same, raid56_rmw_single_phase=1          (control)
#   deg-norepair   same, raid56_no_repair_on_fault=1        (control)
#   hea            healthy, Q failing during the overwrite  (expect: SILENT)
#   hea-single / hea-norepair                                (controls)
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: r6deg.sh <kernel> [arm...]}
shift
ARMS=${*:-"deg deg-single deg-norepair hea hea-single hea-norepair"}
NDEV=5
PROFILE=raid6:raid1c3
HERE=$(cd "$(dirname "$0")" && pwd)
DUMP=$HERE/../../../raid56_wib_dump.py
mkdir -p $T/umltest
cp $HERE/init-r6deg.sh $T/umltest/init-open-r6deg.sh.$$ && mv -f $T/umltest/init-open-r6deg.sh.$$ $T/umltest/init-open-r6deg.sh	# atomic: a guest may be reading it
cp $HERE/r6deg_layout.py $T/umltest/r6deg_layout.py
cp $HERE/r6exp.py $T/umltest/r6exp.py
ulimit -c 0

boot() {	# logname mode omit extra   (uses the caller's $D and $tag)
	local lname=$1 mode=$2 omit=$3 extra="${4:-}" ubds=""
	space_ok || return 1
	for d in $(seq 0 $((NDEV-1))); do
		[ "$d" = "$omit" ] && continue
		ubds="$ubds ubd$d=$D/disk$d.img"
	done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-open-r6deg.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw PROFILE=$PROFILE CRASH=0 \
		TAG=$tag MNTDEV=/dev/ubda NDEV=$NDEV \
		OMITTED=$([ "$omit" = none ] || echo "$omit") $extra \
		> $D/log.$lname 2>&1
	echo "boot $lname rc=$?" >> $T/umltest/results.$tag
}

space_ok() {
	local avail=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
	[ "$avail" -ge 4 ] || { echo "LOW DISK: ${avail}G free"; return 1; }
}

arm() {	# name
	local name=$1 variant knob
	case $name in
	deg*) variant=degraded;; hea*) variant=healthy;; lit*) litarm $name; return;;
	f2*) f2arm $name; return;;
	f1b*) f1barm $name; return;;
	*) echo "bad arm $name"; return;;
	esac
	case $name in
	*-single) knob=btrfs.raid56_rmw_single_phase=1;;
	*-norepair) knob=btrfs.raid56_no_repair_on_fault=1;;
	*-clears) knob=btrfs.raid56_refusal_clears_marks=1;;
	*) knob="";;
	esac
	local tag=open-r6deg-$name
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/r6.*.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot prep r6deg_prep none
	local L=$(cat $T/umltest/r6.layout.$tag 2>/dev/null)
	case "$L" in FULL=*) ;; *) echo "  $name: no layout ($L)"; return;; esac
	eval "$L"
	echo "  $name: layout $L"
	if [ $variant = degraded ]; then
		boot fault r6deg_fault $Q "X=$D0 FO0=$FO0 $knob"
	else
		boot fault r6deg_fault none "VARIANT=healthy X=$D0 QDEV=$Q FO0=$FO0 $knob"
	fi
	python3 $DUMP $D/disk*.img > $D/wibdump.after-fault 2>&1
	boot check-ro r6deg_check none "CHECK=ro FO0=$FO0"
	boot check-scrub r6deg_check none "CHECK=scrub FO0=$FO0"
	python3 $DUMP $D/disk*.img > $D/wibdump.after-scrub 2>&1
	rm -f $D/disk*.img	# scored from the logs; the images are not needed
}

# The literal scenario: a device fails briefly (stale columns, repair kept
# off), unmount, mount degraded with a DIFFERENT device omitted, then writes
# into those stripes and queued repairs while the Q device of stripe 0 fails
# writes; then every device back, recovery, scrub, check every block.
#   lit            as is
#   lit-single     raid56_rmw_single_phase=1 in the degraded boot (control)
#   lit-norefuse   raid56_rmw_no_refuse=1 in the degraded boot (control)
litarm() {	# name
	local name=$1 knob
	case $name in
	*-single) knob=btrfs.raid56_rmw_single_phase=1;;
	*-norefuse) knob=btrfs.raid56_rmw_no_refuse=1;;
	*) knob="";;
	esac
	local tag=open-r6deg-$name
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/r6.*.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot prep r6degL_prep none "btrfs.raid56_no_repair_on_fault=1 KSTRIPES=8"
	local L=$(cat $T/umltest/r6.layout.$tag 2>/dev/null)
	case "$L" in FULL=*) ;; *) echo "  $name: no layout ($L)"; return;; esac
	eval "$L"
	echo "  $name: layout $L; X=$D0 omitted=$D2 failing=$Q"
	boot degraded r6degL_degraded $D2 "QDEV=$Q $knob"
	python3 $DUMP $D/disk*.img > $D/wibdump.after-degraded 2>&1
	boot check r6degL_check none ""
	python3 $DUMP $D/disk*.img > $D/wibdump.after-check 2>&1
	rm -f $D/disk*.img
}

# F2: the repair's own flush fails, then a crash in a later write.
#   f2        W fails writes (and flushes) during the repairing write
#   f2-ctl    NOFLUSHFAIL=1: the same without W's failure (control)
f2arm() {	# name
	local name=$1 extra=""
	case $name in *-ctl) extra="NOFLUSHFAIL=1";; *-keepclear) extra=btrfs.raid56_persist_fail_keeps_clear=1;; esac
	local tag=open-r6deg-$name
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/r6.*.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot prep r6deg_prep none
	local L=$(cat $T/umltest/r6.layout.$tag 2>/dev/null)
	case "$L" in FULL=*) ;; *) echo "  $name: no layout ($L)"; return;; esac
	eval "$L"
	echo "  $name: layout $L; X=$D0 W=$D1 omitted Q=$Q"
	boot fault r6degF2_fault $Q "X=$D0 W=$D1 FO0=$FO0 $extra"
	python3 $DUMP $D/disk*.img > $D/wibdump.after-crash 2>&1
	boot check-ro r6degF2_check $Q "DEGRADED=1 FO0=$FO0"
	rm -f $D/disk*.img
}

# F1b: a refused rbio clears the mark of a missing data column.
#   f1b          as above (expect: the acknowledged 'B' reads back as the
#                pre-degraded content once the device is back)
#   f1b-single   raid56_rmw_single_phase=1 (control)
#   f1b-heal     X healed before the whole-column write (control)
f1barm() {	# name
	local name=$1 extra=""
	case $name in
	*-single) extra=btrfs.raid56_rmw_single_phase=1;;
	*-heal) extra=CONTROL=heal;;
	*-clears) extra=btrfs.raid56_refusal_clears_marks=1;;
	esac
	local tag=open-r6deg-$name
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/r6.*.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot prep r6deg_prep none
	local L=$(cat $T/umltest/r6.layout.$tag 2>/dev/null)
	case "$L" in FULL=*) ;; *) echo "  $name: no layout ($L)"; return;; esac
	eval "$L"
	echo "  $name: layout $L; X=$D0 omitted Y=$D1"
	boot fault r6degF1b_fault $D1 "X=$D0 FO0=$FO0 $extra"
	python3 $DUMP $D/disk*.img > $D/wibdump.after-fault 2>&1
	boot check-ro r6degF2_check none "FO0=$((FO0 + 65536))"
	rm -f $D/disk*.img
}

summary() {	# name
	local tag=open-r6deg-$1 D=$T/umltest/open-r6deg-$1
	local ack=$(grep -ho "OVERWRITE_[A-Z]*" $D/log.fault 2>/dev/null | head -1)
	local st=$(grep -ho "after repair attempt: .*" $D/log.fault 2>/dev/null | grep -o "repair_ok [0-9]*\|repair_failed [0-9]*\|repair_queued [0-9]*" | tr '\n' ' ')
	local inm=$(grep -ho "IN_MOUNT_READ [A-Za-z/]*" $D/log.fault 2>/dev/null | head -1)
	case $1 in f2*|f1b*)
		echo "  $1: $(grep -ho 'write [0-9]* [+0-9]*acked=[01]' $D/log.fault 2>/dev/null | tr '\n' ',') $(grep -c 'crash injection' $D/log.fault 2>/dev/null) crash | $(grep -ho 'TARGET [A-Z]*' $D/log.check-ro 2>/dev/null) | $(grep -ho 'NOCOW_ALL.*' $D/log.check-ro 2>/dev/null) | splats=$(grep -l KERNEL_SPLAT $D/log.* 2>/dev/null | wc -l)"
		return;;
	esac
	case $1 in lit*)
		echo "  $1: prep: $(grep -c 'acked=1' $D/log.prep 2>/dev/null) acked | degraded writes acked: $(grep -c 'col 1.*acked=1' $D/log.degraded 2>/dev/null) refused: $(grep -c 'col 1.*acked=0' $D/log.degraded 2>/dev/null) | $(grep -ho 'DEGRADED_READ.*' $D/log.degraded 2>/dev/null) | after scrub: $(grep -ho 'NOCOW_ALL.*' $D/log.check 2>/dev/null) $(grep -ho 'CSUMFILE.*' $D/log.check 2>/dev/null) | splats=$(grep -l KERNEL_SPLAT $D/log.* 2>/dev/null | wc -l)"
		return;;
	esac
	local ro=$(grep -ho "NOCOW target=[A-Z]*.*" $D/log.check-ro 2>/dev/null | head -1)
	local sc=$(grep -ho "NOCOW target=[A-Z]*.*" $D/log.check-scrub 2>/dev/null | head -1)
	local splat=$(grep -l KERNEL_SPLAT $D/log.* 2>/dev/null | wc -l)
	echo "  $1: ${ack:-?} | $st| ${inm:-?} | ro: ${ro:-?} | rw+scrub: ${sc:-?} | splats=$splat"
}

for a in $ARMS; do arm $a; done
echo "== results =="
for a in $ARMS; do summary $a; done
