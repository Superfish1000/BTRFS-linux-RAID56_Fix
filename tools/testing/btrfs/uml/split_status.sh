#!/bin/bash
# Does a degraded RAID5 read return a reconstruction it already rejected?
#
#   split_status.sh <kernel> [rounds] [ndev] [fail-device]
#
# btrfs splits a bio at the stripe boundary, so a read that crosses one becomes
# two: a clone for the first half and the original, advanced, for the rest.
# Both complete into btrfs_bio_end_io(), which keeps the first error in
# bbio->status and copies it into bbio->bio.bi_status for the caller.
#
# It used to make that copy only when the half finishing LAST had succeeded.
# That is exactly backwards for a degraded array.  The half whose device is
# present is read straight off it; the half whose device is missing is
# reconstructed by raid56_parity_recover() on a workqueue, so it comes back
# later -- and when its reconstruction failed its checksum, the error it
# carried was dropped and the status the caller saw was the other half's OK.
#
# end_bbio_data_read() reads one status for the whole original bio, so every
# folio was marked uptodate, including the half that failed.  The read returned
# the rejected reconstruction as the file's content, complete, at the right
# length, with no error, on fully checksummed data.
#
# The scenario is the ordinary flakey one: write files while a device fails
# writes, crash, recover, then mount with that device omitted so its columns
# have to come from a parity that no longer describes them.  Every such file
# must fail to read.  The measurement is verify_manifest()'s "silent" count:
# files that read back COMPLETE, at the right size, with content nobody wrote.
#
# Two arms.  The control sets split_bio_status_legacy=1, restoring the old
# rule.  A clean fixed arm means nothing unless the control is dirty, and the
# control needs a file whose extent straddles a stripe boundary with the
# earlier half on the missing device -- which is common but not certain in any
# one boot, so both arms run several rounds and the counts are summed.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: split_status.sh <kernel> [rounds] [ndev] [fail-device]}
ROUNDS=${2:-3}
NDEV=${3:-4}
FAIL=${4:-2}
HERE=$(cd "$(dirname "$0")" && pwd)

# Sum "silent=N" across every degraded boot of one arm.
arm() {	# tag legacy -> echoes "<silent> <bad>"
	local tag=$1 legacy=$2 r silent=0 bad=0 s b
	for r in $(seq 1 $ROUNDS); do
		EXTRA_CMDLINE=$([ "$legacy" = 1 ] && echo "btrfs.split_bio_status_legacy=1") \
		BTRFS_TEST_DIR=$T $HERE/dmfail34.sh "$KERNEL" "$tag-$r" flakey \
			raid5:raid1 rw "$NDEV" "$FAIL" > $T/umltest/out.$tag-$r 2>&1
		s=$(grep -haoE 'DEGRADED_MANIFEST total=[0-9]+ bad=[0-9]+ silent=[0-9]+' \
			$T/umltest/$tag-$r/log.degraded.omit* 2>/dev/null |
			sed 's/.*silent=//' | paste -sd+ | bc)
		b=$(grep -haoE 'DEGRADED_MANIFEST total=[0-9]+ bad=[0-9]+' \
			$T/umltest/$tag-$r/log.degraded.omit* 2>/dev/null |
			sed 's/.*bad=//' | paste -sd+ | bc)
		silent=$((silent + ${s:-0}))
		bad=$((bad + ${b:-0}))
	done
	echo "$silent $bad"
}

echo "== control: split_bio_status_legacy=1 =="
read -r ctl_silent ctl_bad <<<"$(arm sps-ctl 1)"
echo "  files damaged: $ctl_bad   of those returned silently: $ctl_silent"
echo "== with the error kept =="
read -r fix_silent fix_bad <<<"$(arm sps-fix 0)"
echo "  files damaged: $fix_bad   of those returned silently: $fix_silent"
echo

if [ "${ctl_bad:-0}" -eq 0 ] 2>/dev/null || [ "${fix_bad:-0}" -eq 0 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- an arm damaged nothing, so neither arm's"
	echo "        silent count says anything"
	exit 2
fi
if [ "${ctl_silent:-0}" -eq 0 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- the control returned nothing silently, so a"
	echo "        clean fixed arm proves nothing.  Raise the round count."
	exit 2
fi
if [ "${fix_silent:-0}" -ne 0 ] 2>/dev/null; then
	echo "RESULT: FAIL -- $fix_silent file(s) still read back complete with"
	echo "        content nobody wrote and no error"
	exit 1
fi
echo "RESULT: PASS -- control returned $ctl_silent damaged file(s) as if they"
echo "        were intact, the fixed arm returned 0; both still detect the"
echo "        damage itself ($ctl_bad and $fix_bad files unreadable)"
