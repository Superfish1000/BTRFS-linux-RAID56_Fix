#!/bin/bash
# Run the policy x scenario-family matrix of policy_model.py, the negative
# controls and the variant runs, in parallel (nice 10; the CPUs are shared).
#   ./run_all.sh [jobs]        default 3 parallel jobs
# Output: out/<run>.txt (one RESULT line + traces), then summary.md.
cd "$(dirname "$0")"
JOBS=${1:-3}
TL=${TL:-1700}          # seconds per run before it stops (reported TRUNCATED)
mkdir -p out
: > jobs.txt

# family name | flags (RAID5, 2 full stripes, one sector per column unless given)
FAMILIES=(
"transient|--bad transient --crash 2 --remount 1 --repair --scrub --depth 10"
"persistent|--bad persistent --crash 2 --remount 2 --repair --replace --scrub --depth 10"
"flush|--flush --crash 2 --remount 1 --repair --scrub --depth 10"
"flush_unnamed|--flush --unnamed-flush --crash 1 --remount 1 --scrub --depth 9"
"crash_loss|--detach 1 --crash 2 --remount 1 --scrub --depth 10"
"missing_return|--detach 1 --ret --crash 1 --remount 2 --scrub --replace --depth 10"
"missing_return_full|--detach 1 --ret --cap 1 --crash 1 --remount 2 --scrub --replace --depth 10"
"missing_return_twice|--detach 2 --ret --crash 1 --remount 3 --scrub --depth 9"
"persistent_full|--bad persistent --cap 1 --crash 1 --remount 1 --repair --replace --scrub --depth 10"
"replay_full|--bad persistent --replay --cap 1 --crash 1 --remount 1 --scrub --depth 9"
"replay_full_transient|--bad transient --replay --cap 1 --crash 1 --remount 1 --scrub --depth 9"
"replace|--bad persistent --detach 1 --replace --crash 1 --remount 1 --depth 8"
"raid6_second|--raid 6 --detach 2 --crash 1 --remount 1 --scrub --depth 8"
"raid5_second_fault|--bad persistent --detach 1 --overfault --crash 1 --remount 1 --replace --depth 7"
"nospare_persistent_full|--bad persistent --cap 1 --crash 1 --remount 1 --repair --scrub --no-spare --depth 10"
"raid6_bad_missing_full|--raid 6 --bad persistent --detach 1 --cap 1 --crash 1 --remount 1 --replace --depth 7"
"cap2_missing_return|--stripes 3 --cap 2 --detach 1 --ret --crash 0 --remount 2 --scrub --depth 7"
"meta_persistent_full|--meta --bad persistent --cap 1 --crash 1 --remount 1 --replace --depth 9"
)
POLICIES="OLD A CUR A2 B C CB CTM"

for fam in "${FAMILIES[@]}"; do
	name=${fam%%|*}
	flags=${fam#*|}
	for pol in $POLICIES; do
		datas="nodatasum csum"
		case "$flags" in *--meta*) datas="csum";; esac
		for data in $datas; do
			echo "M_${name}_${pol}_${data}|--policy $pol --data $data $flags" >> jobs.txt
		done
	done
done

# fully strict variants (refuse an RMW that must rebuild an absent column)
# and the narrow in-flight refinement, on the families where they matter
for fam in "${FAMILIES[@]}"; do
	name=${fam%%|*}
	flags=${fam#*|}
	case "$name" in
	persistent|crash_loss|missing_return_full|replay_full) ;;
	*) continue;;
	esac
	for pol in A C; do
		echo "V_${name}_${pol}+SD_nodatasum|--policy $pol --data nodatasum --mut strict_degraded $flags" >> jobs.txt
		echo "V_${name}_${pol}+NI_nodatasum|--policy $pol --data nodatasum --mut narrow_inflight $flags" >> jobs.txt
		echo "V_${name}_${pol}+SD+NI_nodatasum|--policy $pol --data nodatasum --mut strict_degraded --mut narrow_inflight $flags" >> jobs.txt
	done
	for pol in A2 CTM; do
		echo "V_${name}_${pol}+SD+NI_nodatasum|--policy $pol --data nodatasum --mut strict_degraded --mut narrow_inflight $flags" >> jobs.txt
	done
done

# negative controls: each must show its failure class (checked by summarize.py)
fl() { for fam in "${FAMILIES[@]}"; do [ "${fam%%|*}" = "$1" ] && echo "${fam#*|}"; done; }
CONTROLS=(
"K_old_write_hole|SILENT_WRONG|--policy OLD --data nodatasum $(fl crash_loss)"
"K_old_stale_member|SILENT_WRONG|--policy OLD --data nodatasum $(fl transient)"
"K_old_flush|SILENT_WRONG|--policy OLD --data nodatasum $(fl flush)"
"K_cur_degraded_evict|SILENT_WRONG|--policy CUR --data nodatasum $(fl missing_return_full)"
"K_cur_replay_evict|SILENT_WRONG|--policy CUR --data nodatasum $(fl replay_full)"
"K_cur_unnamed_flush|SILENT_WRONG|--policy CUR --data nodatasum $(fl flush_unnamed)"
"K_b_degraded_hole|LOST_ACKED_GONE|--policy B --data nodatasum $(fl persistent)"
"K_c_degraded_hole|LOST_ACKED_GONE|--policy C --data nodatasum $(fl persistent)"
"K_c_trust_unverified|SILENT_WRONG|--policy C --mut c_trust_unverified --data nodatasum $(fl persistent)"
"K_ctm_trust_unverified|SILENT_WRONG|--policy CTM --mut c_trust_unverified --data nodatasum $(fl missing_return)"
"K_a_evict_naming|SILENT_WRONG|--policy A --mut evict_naming --data nodatasum $(fl persistent_full)"
"K_a2_no_taint|SILENT_WRONG|--policy A2 --mut a2_no_taint --data nodatasum $(fl missing_return_full)"
"K_a_no_suspect|SILENT_WRONG|--policy A --mut no_suspect --data nodatasum $(fl crash_loss)"
"K_a_read_trust_torn|SILENT_WRONG|--policy A --mut read_trust_torn --data nodatasum $(fl crash_loss)"
"K_b_read_trust_torn|SILENT_WRONG|--policy B --mut read_trust_torn --data nodatasum $(fl persistent)"
"K_a_absent_par_unnamed|SILENT_WRONG|--policy A --mut absent_par_unnamed --data nodatasum $(fl missing_return_twice)"
"K_a_ack_unnamed|SILENT_WRONG|--policy A --mut ack_unnamed --data nodatasum $(fl flush_unnamed)"
"K_b_no_record|SILENT_WRONG|--policy B --mut b_no_record --data nodatasum $(fl persistent)"
"K_a_missing_wedge|REFUSED_WRITE|--policy A --data nodatasum $(fl missing_return_full)"
"K_a_replay_down|RO|--policy A --data nodatasum $(fl replay_full)"
"K_old_csum_write_hole|LOST_ACKED_GONE|--policy OLD --data csum $(fl crash_loss)"
"K_a_nospare_stuck|STUCK|--policy A --data nodatasum $(fl nospare_persistent_full)"
)
for c in "${CONTROLS[@]}"; do
	name=${c%%|*}; rest=${c#*|}; want=${rest%%|*}; flags=${rest#*|}
	echo "${name}|${flags}" >> jobs.txt
	echo "${name} ${want}" >> controls.want.tmp
done
sort -u controls.want.tmp > controls.want; rm -f controls.want.tmp

run_one() {
	line="$1"
	name=${line%%|*}
	flags=${line#*|}
	[ -s "out/$name.txt" ] && grep -q '^RESULT' "out/$name.txt" && exit 0
	nice -n 10 python3 policy_model.py $flags --name "$name" --time "$TL" --max-rss-mb 2500 --nosymm \
		--classes SILENT_WRONG,LOST_ACKED_GONE,LOST_ACKED_UNREACHABLE,STUCK,REFUSED_READ --maxclasses 4 \
		--traces > "out/$name.txt.tmp" 2>&1 && mv "out/$name.txt.tmp" "out/$name.txt"
}
export -f run_one
export TL
echo "$(wc -l < jobs.txt) runs, $JOBS in parallel"
tr '\n' '\0' < jobs.txt | xargs -0 -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {}
python3 summarize.py > summary.md
echo done
