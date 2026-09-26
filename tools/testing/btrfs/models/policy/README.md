# RAID5/6 write-failure policy comparison

Exhaustive state-space models comparing what a RAID5/6 array with the
write-intent log does when a disk fails writes, a flush is lost, the
machine crashes, or a disk goes missing and comes back.  Each policy is a
switch of one model, so the same fault schedules are run against all of
them.

Policies (`--policy`):

- `OLD`: upstream btrfs without the log.
- `A`: strict refusal: never drop a record that protects acknowledged
  data; refuse the write (EIO, read-only for metadata) instead.
- `CUR`: the kernel at stage 0 (A, except that a full log drops records
  while a device is missing, during tree-log replay, and for torn-only
  records).
- `A2`: A, except that records naming only a missing device may be dropped
  if that device is then distrusted (tainted).
- `B`: per-stripe degrade (treat a stripe's failing disk as missing and
  keep writing).
- `C`, `CB`, `CTM`: the md-style "failed for writes" device state of the
  stage-1 design, in three readings.

Start with `COMPARISON.md` (the findings, ranked, with the fixes each
needs).  `RESULTS.md` and `summary.md` hold the shared matrix;
`reports/` holds each attacker's report and the fidelity check of the
model against the kernel code; `attack/` holds the attackers' model
variants.  `policy_model_fid.py` is the model with the fidelity
critic's three corrections (`--fid F1,F2,F3`; `--fid none` is the
original).

Paths in the reports (`<scratch>/...`) refer to the directory the runs
were made in; the model files named there are the ones in this
directory and in `attack/`.  Example:

    python3 policy_model.py --policy A --bad persistent --depth 10

Runs are CPU- and memory-heavy; run_all.sh has the full matrix.
