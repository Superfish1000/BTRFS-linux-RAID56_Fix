#!/bin/bash
# Sweep the RAID56 redundancy model over its configuration space.
run() { printf "%-62s " "$*"; python3 raid56_redundancy_model.py "$@" 2>&1 | grep -E "VIOLATION|OK:" | head -1; }
echo "### fixed accounting must be clean everywhere"
for parity in 1 2; do for depth in 3 4; do
  run --parity $parity --depth $depth
  run --parity $parity --depth $depth --replacing
  run --parity $parity --depth $depth --missing 0
done; done
echo
# The two documented residual exposures (docs/superpowers/needs-direction.md).
# --in-place only shows under --strict: an overwrite that fails destroys the
# data it was overwriting, and outside --strict a failed write is allowed to
# do that.  Run without it, an --in-place row can never violate, so a check
# that expects one to is checking nothing.
echo "### residual exposures (expected to violate)"
for parity in 1 2; do for depth in 3 4; do
  run --parity $parity --depth $depth --in-place --strict
  run --parity $parity --depth $depth --nodatasum
done; done
echo
# Realistic array widths.  Everything above runs at the default --data 2, i.e.
# a 3-disk RAID5 and a 4-disk RAID6, which is NOT what the measured arrays look
# like (nr_data 3 and 7).  At three or more data stripes the model reports
# acknowledged loss; see docs/superpowers/needs-direction.md.  Listed
# separately so the result is visible rather than absent.
echo "### wider arrays"
for data in 3 4 5; do for parity in 1 2; do
  run --data $data --parity $parity --depth 3
done; done
# The same widths on a DEGRADED array.  Every row above runs on a healthy one,
# so the sweep never asked whether width and a missing device interact -- and
# for RAID6 they do: the loss reproduces at nr_data >= 3 with one device gone,
# which is a case the flat de-rate is suppressed in by construction and so
# cannot close.  See needs-direction.md item 5.
for data in 3 4; do for parity in 1 2; do for mis in 0 3; do
  run --data $data --parity $parity --depth 3 --missing $mis
done; done; done
echo
# The de-rate is a PROPOSAL for the wider-array loss above, not something the
# kernel does: rbio_max_errors() is the flat profile tolerance and nothing
# consults the log before a write.  Run here to show what each variant would
# buy, so the entry in needs-direction.md rests on a result rather than an
# argument.  See that file for why neither is applied.
echo "### de-rate proposals for the wider-array loss (not implemented)"
for data in 3 4; do
  run --data $data --parity 1 --depth 3 --flat-sticky-derate
  run --data $data --parity 1 --depth 3 --counted-sticky-derate
  run --data $data --parity 2 --depth 3 --counted-sticky-derate
  # The third formulation: de-rate by what the LOG RECORDS about the stripe
  # (btrfs_wib_stripe_state's stale_cols and bad_parity), not by the sticky
  # bit and not by comparing parity against disk.  Unlike the counted variant
  # it needs no device read, so it is implementable on the write path; unlike
  # the flat variant it needs no suppression while a device is missing, so it
  # also closes the degraded rows above.
  run --data $data --parity 1 --depth 3 --recorded-derate
  run --data $data --parity 2 --depth 3 --recorded-derate
  run --data $data --parity 2 --depth 3 --missing 0 --recorded-derate
  run --data $data --parity 2 --depth 3 --missing 3 --recorded-derate
done
echo
# The flat variant was only ever run at --parity 1, and needs-direction.md
# item 5 generalised that to "for both parities".  It does not hold: the RAID6
# counterexample begins with a device loss, and the flat de-rate suppresses
# itself whenever a device is missing, so it is switched off exactly where the
# loss is reachable.  Its own suppression is what it cannot close.
echo "### flat de-rate limits: what it does NOT close (expected to violate)"
for data in 3 4 5; do
  run --data $data --parity 2 --depth 3 --flat-sticky-derate
done
# And the cost: the flat variant refuses writes the array could still serve,
# already at the default width where the recorded variant does not.
run --parity 1 --depth 3 --availability --flat-sticky-derate
run --parity 2 --depth 3 --availability --flat-sticky-derate
# What the flat variant does to a degraded array without the suppression that
# hides it: the entry in needs-direction.md says it goes read-only one stripe
# at a time, and this is the row that shows it rather than arguing it.
run --parity 1 --depth 3 --missing 0 --availability --flat-sticky-derate-degraded
echo
echo "### recorded de-rate: the availability it does and does not cost"
run --parity 1 --depth 3 --availability --recorded-derate
run --parity 2 --depth 3 --availability --recorded-derate
for data in 3 4; do for parity in 1 2; do
  run --data $data --parity $parity --depth 3 --availability --recorded-derate
done; done
echo
echo "### each accounting fix reverted must break something"
run --depth 3 --no-missing-faults
run --depth 3 --replacing --replace-inflation
run --depth 3 --replacing --target-aliasing --availability
run --depth 4 --policy upstream
run --depth 4 --policy upstream --replacing
