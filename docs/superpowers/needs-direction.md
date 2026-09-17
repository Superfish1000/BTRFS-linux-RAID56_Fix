# Open questions that need a decision

Things found while working on the RAID5/6 integrity series that are **not**
safe to fix autonomously, because the code is not simply wrong -- there is a
trade-off, a design choice, or a user-visible change involved. Each entry says
what was found, what the choice is, and what it would cost.

Bugs with an unambiguous correct fix are not listed here; those are fixed and
committed directly.

---

## 1. Crash + device loss is still unrecoverable

**What.** The write-intent log records stripe *addresses*, not data. Recovery
recomputes parity from the sectors on disk, which works because after a crash
the data is intact and only parity is stale. If a device dies before recovery
runs, there is no longer enough data to recompute, and the stripe is lost.

**The choice.** Closing it means journalling content, as Linux MD's RAID5
journal does. Measured against the eight-disk aging run: the log currently
writes 234K sectors; journalling the data and parity of every sub-stripe write
would write 1,476K. **Write amplification goes from 16% of RMW traffic to
100%.** MD makes its journal optional for exactly this reason.

**Options.** (a) Leave as is and document the limit. (b) Add optional
journalling behind a mount option or feature flag. (c) Journal only parity,
which is cheaper but does not close the gap on its own.

---

## 2. BTRFS_STRIPE_LEN is a compile-time constant

**What.** Stranded free space under an immutable-stripe rule is dominated by
the 64 KiB stripe unit, not by the rule. Re-gridding the same aged filesystems:

| | 64 KiB | 16 KiB | 8 KiB | 4 KiB |
|---|---|---|---|---|
| RAID5 8-disk, nr_data=7 | 46.1% | 18.7% | 11.2% | 6.4% |
| RAID5 4-disk @92% full | 47.6% | 16.7% | 9.3% | 5.2% |

The on-disk format already carries `stripe_len` as a per-chunk `__le64` and
mkfs writes it, but `BTRFS_STRIPE_LEN` is `#define SZ_64K` in `volumes.h`, the
kernel never reads the field back, and `tree-checker.c` enforces the constant.

**The choice.** Making it variable is a substantial kernel change and costs the
property that a small read touches a single disk, plus many more rbios per
byte written. Worth it only if the immutable-stripe family is being pursued.

**Cheap next step if wanted.** Rebuild the UML kernel with a smaller
`BTRFS_STRIPE_LEN` and re-run the aging workload, to check the modelled numbers
against a filesystem that actually ran at that granule.

---

## 3. Stripe alignment is silently dropped on the bitmap path (pre-existing)

**What.** `bg->full_stripe_len` is cached at `block-group.c:2421` and passed as
an alignment into `find_free_space()` (`free-space-cache.c:3104`). On the
extent path it is honoured. On the **bitmap** path it is not:
`search_bitmap()` takes no alignment argument, and `find_free_space()` uses the
aligned offset only as a search start, returning whatever
`*offset = i * unit + bitmap_info->offset` finds
(`free-space-cache.c:2079-2087`).

So once a block group's free space has degraded into bitmaps -- which is what
happens as a filesystem ages, exactly when it matters -- stripe-aligned
allocation stops happening, with no indication.

Separately, alignment is applied only `if (*bytes >= align)`
(`free-space-cache.c:2056`), so it never applies to requests smaller than a
full stripe. That one looks deliberate: aligning a 4 KiB request to 448 KiB
would waste the difference.

**Why this is not fixed here.** It is upstream code, not part of this series;
the effect is on performance (more read-modify-writes) rather than correctness;
and honouring alignment inside bitmaps changes allocation behaviour for every
profile, not just RAID5/6.

**The choice.** Leave it; or teach `search_bitmap()` an alignment and measure
the effect on the full-stripe versus sub-stripe ratio, which
`/sys/fs/btrfs/<uuid>/raid56_write_profile` now reports directly.

---

## 5. The model reports acknowledged loss at three or more data stripes

**What.** `sweep.sh` originally never passed `--data`, so every configuration
behind the "fixed accounting is clean everywhere" claim ran at the default of
two data stripes -- a 3-disk RAID5 and a 4-disk RAID6. The arrays actually
measured in this series have `nr_data` 3 and 7. At three or more the model
reports acknowledged loss, for both RAID5 and RAID6:

```
history: ('rmw', (1,2), ('1','2','p0')) ; ('rmw', (2,), ('2',)) ; ('rmw', (1,), ('p0',))
final:   disk=[0,102,2]  parity=[(0,1,101)]  committed=[0,102,101]
```

The second write puts 101 into stripe 2; its data write fails and only the
parity carries the value. One fault against one parity, so the write is
accepted. The third write touches stripe 1 and loses its parity write. Again
one fault, again accepted -- but parity was the only copy of stripe 2, so that
stripe's committed content now exists nowhere.

`nr_data = 2` is clean at depth 5, so this is a width property and not a
search-depth artifact.

**Why it is not obviously a defect.** Each RMW counts faults within its own
rbio, which is what `rbio_max_errors()` is for; neither write individually
exceeds the profile. The stripe is left recorded as sticky in the write-intent
log precisely because it completed with a device error, so the next mount
scrubs it and restores redundancy. The exposure is the window between the
failed write and that scrub.

**The choice.** Whether an RMW should de-rate its fault tolerance when the
stripe it is about to write is already one fault down -- at the cost of failing
writes that today succeed. Or whether this is inherent to a one-fault-tolerant
profile taking two faults, and belongs in the documented exposures instead.

### Both de-rate variants were built and measured

Written, run, and then backed out. `sweep.sh` runs both so the numbers stay
visible.

**Flat (`--flat-sticky-derate`).** `btrfs_wib_has_sticky()` before the writes,
then `rbio_max_errors(rbio) - degraded` after them. Implementable: it needs
only the log's sticky bit. It **does** close the wider-array loss at `--data 3`
and `--data 4`, for both parities.

It also breaks degraded arrays. The sticky bit does not say why it was set, and
a missing device sets one on every write it touches -- but that same fault is
already counted against the next write by the `missing_faults` rule, so
de-rating for it charges one lost equation twice. On a RAID5 with a device
gone that is the entire budget: the first write to a full stripe succeeds and
records it, and every write to that stripe afterwards returns EIO. A degraded
array goes read-only one stripe at a time.

Suppressing the de-rate while any device is missing fixes that particular case
and leaves the real cost: on a healthy array, a stripe that took one transient
write error refuses its next failing write until a scrub clears the record.
That is correct for integrity and is a loss of availability, which is the
trade-off this entry exists to have decided.

**Counted (`--counted-sticky-derate`).** Caps the budget by the stripe's true
remaining margin -- the parities still agreeing with the disk on every
non-stale sector, minus the sectors needing them. Strictly stronger, and *not*
implementable the same way: computing it needs every parity compared against
every sector on disk, which the write path does not have and cannot afford to
read. It is in the model to show what the extra strength would buy, not as a
candidate.

**The trap this walked into.** For a while the model's default policy was the
counted de-rate while the kernel had neither, so the sweep reported "wider
arrays clean" and this entry was deleted as resolved. Nothing in the kernel had
changed. The model's default is now the kernel's behaviour -- no de-rate -- and
a variant has to be asked for by name.

---

## 6. RAID6 Q cross-check: tested, did not reproduce

**The claim.** `recover_verify_q()` rejects a rebuild whenever the two parity
blocks disagree, and they legitimately disagree on stripes holding no committed
data -- so a degraded RAID6 array could not write into fresh space at all.

**Tested.** `uml/degraded_fresh.sh` boots an array degraded from the first
mount and writes into never-written space. On six devices with one omitted,
where the chunk is wide enough for sub-stripe writes to exist at all
(`sub_stripe_writes 23`), all 21 writes plus an in-place nodatacow overwrite
succeeded, with no Q-syndrome warning.

**Why not.** The premise does not hold on these images: unwritten space is
zeros, so P and Q are both zero and agree trivially. The check needs unwritten
space holding *garbage* -- a disk reused from something else -- before P and Q
disagree there.

**What is left.** A narrower question than the original: whether a reused disk
with non-zero content in never-written regions can make a degraded RAID6 refuse
sub-stripe writes. Reproducing it means seeding the images with garbage before
mkfs, which the rig does not currently do. Recorded rather than closed, because
"did not reproduce under the conditions I tried" is not "cannot happen".

Note the first attempt at this test passed while proving nothing: on four
devices with one omitted the chunk is three wide, `nr_data` is 1, and no write
can be sub-stripe -- `sub_stripe_writes` stayed 0. The write profile counters
are what caught that.

---

## 4. Two residual exposures the model checker still reports

Both reproduce in `tools/testing/btrfs/raid56_redundancy_model.py`; the sweep
runs them under their own heading and `regress.sh` diffs the result against
`tools/testing/btrfs/uml/residual-exposures.txt`. Both are documented rather
than fixed, because both need a semantic decision about what a failed sector
write should mean.

**`--in-place --strict`:** a `nodatacow` or prealloc write overwrites
referenced sectors by definition, so committed data is at risk even when the
write covers the full stripe. The log records these (`RBIO_INPLACE_BIT`) so
parity is recoverable, but the overwritten data itself is not. It only shows
under `--strict`, which is the mode that asks whether a *failed* write may
destroy committed data; outside it a failed write is allowed to, so an
`--in-place` row without `--strict` cannot violate and checking one proves
nothing. Reported at both parities and both depths.

**`--nodatasum`:** without checksums a stale sector cannot be told from a good
one, so a reconstruction from a parity that a failed write left behind is
returned as if it were correct. Reported at both parities:

```
history: ('rmw', (1,), ('1', 'p0')) ; ('rmw', (1,), ('p0',))
final:   disk=[0, 101] parity=[(0, 1)] committed=[0, 101]
         read of stripe 1 after losing device 1 returns 1, not 101
```

The second write lands 101 on the data device and loses only its parity write:
one fault, inside the profile's tolerance, so it is accepted. The parity is now
stale -- it still describes the pre-write value. Lose the data device and the
reconstruction from that parity returns 1. With checksums that is a detected
mismatch and the read fails; without them it is returned as data, and the
acknowledged write is silently undone.

**The choice.** Whether a write that spends redundancy should be refused
outright on a `nodatasum` filesystem -- which means any transient write error
on any one device fails the whole write, on the profile that is supposed to
absorb it -- or continue to be accepted with the exposure documented. The
write-intent log narrows the window (the stripe is recorded and the next scrub
repairs it) but does not close it: devices that die before that scrub runs are
enough.

---

---

## 7. Persisting the stale record costs log capacity

**What.** The scrub fix works off the log's `stale` record -- which data column
holds content a failed write left on disk. That record lives only in memory, so
across a mount only `sticky` survives, and `sticky` says a write failed without
saying which side of the stripe is wrong. Until it is persisted, the scrub
protection is per-mount only: the first `btrfs scrub` after a reboot can still
destroy the parity copy.

**The change.** `struct btrfs_wib_disk_entry` grows from 24 bytes. It was 32
when this entry was written and is **48** now
(`static_assert(sizeof(struct btrfs_wib_disk_entry) == 48)`,
`raid56-wib.c:141`): `stale`, `stale_par` and `gen`, with parity staleness
packed into the same bitmap (the block at the full stripe's start encodes parity 0, the next block
parity 1 -- `nr_data` is at least 2, so those bits always belong to the stripe
they describe).

**The cost, measured -- and it is not the one this entry originally named.**
The obvious reading is "less dirty address space tracked", which sounds
harmless. The real cost is on the write path.

The on-disk block is not the in-flight set: `btrfs_wib_build_block()` unions
the live table with `wib->last`, so the block is the running union of every
region marked since the last drop, and drops happen only at transaction commit
(30s by default). When that union no longer fits, `wib_write_block_locked()`
returns `-ENOSPC` and `wib_commit_locked()` falls back to
`wib_flush_and_drop_locked()` -- a `REQ_PREFLUSH` to **every writable device**
plus a FUA block write, up to three times, **with the marking RMW blocked on
it** inside `btrfs_wib_mark()`.

That overflow fires once per `BTRFS_WIB_MAX_ENTRIES` distinct new 4MiB
regions. So the entry size sets the rate of inline device-wide cache flushes
on the write path:

| entry | entries/slot | tracked | flush rate |
|---|---|---|---|
| 24B, today (no stale) | 165 | 660 MiB | 1.00x |
| 32B (+stale) | 124 | 496 MiB | 1.33x |
| 40B (+stale, +stale_par) | 99 | 396 MiB | 1.67x |
| 48B (+stale, +stale_par, +gen) -- **what is implemented** | 82 | 328 MiB | 2.01x |

The last row is computed, not estimated: the header is 128 bytes, so a 4KiB
slot holds `(4096 - 128) / 48 = 82` wide entries against `/ 24 = 165` narrow
ones, and the flush rate is that ratio. The row above it was the state of the
tree when this entry was written and is kept to show the direction of travel --
every field added to the record costs log reach, and the cost is now 2x rather
than the 1.67x this entry was decided on.

The in-memory table is a fixed array sized by the same constant
(`entries[BTRFS_WIB_MAX_ENTRIES]`), so it cannot be tuned independently. More
slots do not help: the two slots are alternating generations of the same
block. A bigger slot has 512KiB of physical headroom but is blocked by
single-page buffers and single-page bios.

**The remedy, designed but not built.** `stale` is zero almost always -- the
design says so itself, which is why `btrfs_wib_stale()` has a lock-free
`nr_stale == 0` fast path. So encode it out of the common case: keep the
24-byte entry, and append a sparse extension array of `{__le32 index, __le64
stale, __le64 stale_par}` (20 bytes) after it, with the count in the header.
With no stale records that is 165 entries and **no regression at all**; with
every entry stale it is 90, slightly worse than the flat 99, which is the
right way round -- the pathological case pays and the normal case does not.

**The choice.** Ship the flat 40-byte entry and its 1.67x flush rate; or build
the sparse encoding first; or leave the record per-mount and accept that the
protection resets at every boot -- which the model prices at 1488 of 8386
RAID5 states destroyed, i.e. the whole fix.

---

## 8. Scrub: skip the stripe, or rebuild it

**What.** `scrub_raid56_parity_stripe()` now declines to regenerate the parity
when the log records a data column of that full stripe stale. Sound, and small:
one early return. The state-machine model scores it 0 on every axis.

But it does not repair. The stronger policy -- rebuild the stale column *from*
the parity, write it back, then regenerate -- is also clean and leaves far
fewer stripes without redundancy: 259 against 973 on RAID5, 2718 against 9705
on RAID6.

**Why it is not done.** It needs a write-back path scrub does not have at that
point. The machinery exists: marking the stale sectors in the scrub stripe's
error bitmap after `scrub_verify_one_stripe()` would make the existing repair
loop reconstruct them through mirror 2 and write them back. It has to go in
`scrub_stripe_read_repair_worker()`, which only knows its own data column, so
it needs a chunk-map lookup to find the full stripe.

**The choice.** Ship the skip and leave 3.5x more stripes unrepaired, or build
the write-back path.

---

## 9. Debug knobs that turn protections off

**What.** Two `CONFIG_BTRFS_DEBUG`-only module parameters were added:

- `raid56_allow_nodatacow` -- turns off both the `chattr +C` refusal and the
  forced copy-on-write fallback.
- `raid56_stale_read_legacy` -- restores the read-path behaviour that ignores
  the rebuild budget.

Both exist because the reproductions cannot otherwise reach the states they
demonstrate: refusing `chattr +C` removes the only way to *build* the
nodatacow-on-RAID5/6 state, and a negative control needs the defect back.

**The choice.** Whether shipping switches that restore known defects, even in
debug builds, is acceptable. The alternative is a second kernel build per
control, which is what made negative controls expensive enough to skip -- and
skipping them is how five claims got made and withdrawn in this series.

---

## 10. `chattr +C` on RAID5/6 now fails

**What.** `check_fsflags_compatible()` returns `-EPERM` for `FS_NOCOW_FL` when
the RAID56 incompat bit is set, matching the existing zoned precedent. Anything
that scripts `chattr +C` on a RAID5/6 array -- VM image directories, database
data directories, `systemd-nspawn` machine trees -- starts failing.

**The choice.** Refusal, which is what the zoned case does and what the user
asked for over silently substituting different behaviour; or a warning plus the
forced copy-on-write, which keeps those scripts working while quietly changing
what they get.

---

## 11. A crash on a failing device leaves files reading back complete and wrong

**What.** The `flakey` scenario -- background writers, one device failing
every write, then a crash, then probes with one device omitted -- reports two
classes of damaged file, and `regress.sh` now separates them:

- **read failed** (4 to 19 per run): the read errors and `md5sum` prints
  nothing. This is item 1 of this document: the log records stripe addresses,
  not content, so a stripe whose data is gone cannot be rebuilt, and the
  kernel says so.
- **wrong data** (0 to 5 per run): the read SUCCEEDS and returns a file of
  exactly the right length whose content differs from what was fsync'd. No
  checksum error is raised.

**Why the second one should not be possible.** The writers use plain `dd`, so
the data is checksummed. Metadata is `raid1`, so the checksum tree is not
touched by the RAID5 degradation. Each file is created exactly once, so there
is no earlier version to roll back to. On those three facts a wrong
reconstruction has to fail its checksum and the read has to error. It does
not, so one of the three is false, and which one is not yet established.

Every mismatching file measured is at exactly its expected size --
45056/45056, 57344/57344, 28672/28672, 53248/53248, 61440/61440, 32768/32768
-- so it is not truncation, which was the obvious explanation and is dead.

**Not this series.** It reproduces with `noraid56_write_intent`, and more
often, not less:

| | wrong data | read failed |
|---|---|---|
| log on | 1, 1, 2, 3, 3 | 0, 5, 7, 9, 11 |
| log off | 0, 3, 5, 7 | 4, 6, 11, 19 |

An earlier reading called the log implicated, on the strength of one run per
arm that happened to give 3 against 0. Repeating the log-off arm gave 3, 5 and
7, so that was noise -- and the fuller picture points the other way. It also
predates every commit in this night's work.

**Reproduce.** `BTRFS_TEST_DIR=... tools/testing/btrfs/uml/dmfail34.sh
<kernel> <tag> flakey raid5:raid1 rw 4 2`

**The choice.** Whether to chase this now -- it is an upstream-shaped bug in
the crash path rather than part of the write-hole work -- or record it and
carry on with the series.

---

## 12. A recorded stripe never retires, and persisting @stale inherits that

**What.** Every stripe the log records because of a device error is kept
recorded **forever**, and its parity is never regenerated by recovery. Traced
in the code, not inferred:

- `stale` is a strict subset of `sticky`, so
  `trusted = !wib_pending_has_error(wib, start, len)` is false for any stripe
  with an error record (`raid56-wib.c`, in `btrfs_wib_recover()`).
- `wib_recover_one()` with `trusted == false` ends
  `if (!trusted) { st->kept++; return 1; }` -- it returns 1 unconditionally.
- `return 1` is the re-arm branch, so `btrfs_wib_add_sticky()` puts the record
  straight back. The drop path is never taken.
- `btrfs_wib_recover_after_replay()` passes `trusted = false` literally, so its
  `if (ret == 0) btrfs_wib_clear_sticky()` is dead code for any stripe whose
  chunk still exists. The only reachable clears there are "the chunk is gone"
  and one corner case.
- ~~`scrub_raid56_parity_stripe()` reads the record and returns without touching
  the log, so a user scrub does not retire it either.~~ **No longer true.** It
  now ends in `btrfs_wib_clear_sticky()` (`scrub.c:2811`), correctly gated on
  the repair write-back having landed -- a stripe repaired in memory whose
  write-back failed keeps its record. So a user scrub *is* a live clearer, and
  the item's "never retires" headline holds only for recovery, not for scrub.

The only live clearer is `rmw_update_stale_data()`, and it is triple-gated: the
RMW must be *logged* (sub-stripe, or in-place -- and in-place on RAID5/6 is now
refused), must have supplied data for that column, and must have taken no error
on it. A full-stripe CoW write over the same range sets `logged = false` and
clears nothing.

**This is pre-existing, not caused by persisting the record.** `sticky` already
behaved this way. What persistence changes is that the *scrub* skip becomes
permanent too: before, `stale` died at unmount and the next scrub would
regenerate the parity -- destroying data, which is why it was fixed, but at
least the record retired.

**Why it matters now.** The combination shipped is "persist the record" plus
"scrub skips a stale stripe". Together they mean a stripe that takes one
transient write error keeps its degraded redundancy until something happens to
sub-stripe-write that exact column again. On cold data, that is never.

**The fix is the one already costed in item 8.** Skipping is what makes the
record permanent; **rebuilding retires it**. If the scrub reconstructs the
stale column from the parity, writes it back and then regenerates the parity,
the data on disk is correct again and the record can be cleared. So item 8 is
not an optimisation -- it is what makes persistence terminate.
`btrfs_scrub_raid56_full_stripe()` already documents itself as "repair the bad
ones from the parity and write them back"; that path simply never fires for a
sector with no checksum, because nothing marks it bad. The stale record is
exactly the thing that could.

**The choice.** Build the rebuild path and let persistence terminate; or ship
as is and accept that an error-recorded stripe stays without regenerated
parity indefinitely; or make the scrub retire the record after skipping it,
which returns to destroying data and is not a real option.

**Smaller findings from the same trace, both fixed:**

- `wib_dropped_bits()` measured change as `bitmap | error`, and `stale` is a
  subset of `error`, so a stale bit clearing on its own looked like no change.
  A "no longer stale" record could become durable before the write that made it
  non-stale. It now counts as a dropped bit, which forces the flush first.
- `struct btrfs_wib_entry` still documented `@stale` as "Not persisted", in the
  exact place a reviewer looks to answer this question.

**Not fixed, recorded:** `wib_readd_stale()` silently drops the record when
`btrfs_wib_add_sticky()` could not find room; a stale verdict survives runtime
chunk removal until the next mount and could in principle be inherited by a new
block group at the same logical address; and on a read-only mount
`wib_readd_stale()` never runs, so the record is invisible for that mount --
which is no worse than upstream, but is not protection either.

---

## 13. The stale record mostly fires for data that has a checksum

**What.** `rmw_update_stale_data()` marks a column stale for **any** logged
sub-stripe write that took an error. There is no nodatacow condition on it.
And since `chattr +C` is now refused on RAID5/6 (item 10), the producers left
in a non-debug kernel are mostly sub-stripe CoW data writes and **metadata
tree blocks on a RAID5/6 metadata profile** -- exactly the content that *does*
carry a checksum.

The record's entire justification is "for data with no checksum this is the
only thing that knows". For checksummed content scrub already knows: it
verifies the sector, finds the mismatch, rebuilds it from the parity and
writes it back. The record adds nothing there -- and the scrub skip built on
it actively costs, because it declines to regenerate a parity scrub was
perfectly capable of fixing.

Unchecksummed data has not disappeared: an inode marked `+C` before the
refusal existed still implies NODATASUM, and its writes are unchecksummed even
once forced to copy-on-write. So the record still has real work to do. It just
is not the common case any more.

**The refinement.** Let the record gate the scrub only for sectors that have
no checksum. Scrub knows which those are -- it already carries the csum
bitmap per data stripe. Where everything in the stripe is checksummed, scrub's
own verification is authoritative and it should proceed normally.

That removes most of item 12's permanence in production, because the stripes
it would otherwise pin open are the checksummed ones scrub can finish itself.

**The choice.** Build the csum gate; or build the rebuild path of item 8,
which subsumes it; or leave the skip blunt and accept that a metadata stripe
that took one transient write error never has its parity regenerated again.

---

## 14. Choosing the on-disk layout per block, and what it fixed

Recorded because the first implementation had all three of these and none was
found by a test.

Persisting `stale` widened the on-disk entry from 24 to 40 bytes, and the
first version stamped the new format on **every** block. That did three things
nobody asked for:

- **Downgrade broke for every filesystem.** A kernel predating the format
  rejects any block with a nonzero flags field -- correctly, since it cannot
  know the stride -- and a rejected log means the stripes it covers are never
  recovered. Stamping the wide format unconditionally imposed that on
  filesystems that had never had a stale record in their lives.
- **The write path paid the flush cost unconditionally**, 99 entries against
  165 before the on-disk union overflows and `btrfs_wib_mark()` waits on a
  `REQ_PREFLUSH` to every device (item 7).
- **The first mount after upgrading could drop records.** A format-1 block may
  legally carry 165 entries; the new kernel accepts and loads all of them, and
  then cannot fit them into a 99-entry wide block.

All three are gone now: `btrfs_wib_build_block()` picks the narrowest layout
that can say what the block has to say, and nothing stale -- the overwhelmingly
common case, and the reason `btrfs_wib_stale()` has a lock-free fast path --
means the narrow one.

Two things that had to follow. The in-memory table is now sized by its own
constant (`BTRFS_WIB_NR_ENTRIES`), because how many regions a mount can track
has nothing to do with how wide an entry is on disk, and letting the narrow
format set it silently shrank live capacity from 165 to 99. And the helpers
that walk the kernel's own last block -- `wib_dropped_bits()`,
`btrfs_wib_block_drops()`, `wib_readd_dropped()` -- used to be stride-safe by
construction and are not any more, since the same kernel now writes both
layouts; they all decode rather than index.


---

## 15. Protecting an ambiguous stripe: what two adversarial rounds established

Design question from the user: during a scrub, when a stripe is found AMBIGUOUS,
clone its bytes to an external recovery drive plugged in for the purpose, and
prevent writes to that stripe meanwhile, so a post-scrub utility can test
rebuild configurations and recover the file.

Two workflows ran. **Both hit a spend limit and neither produced a synthesis**,
so what follows is the completed subset, not a finished analysis.

### What actually completed

| angle | lenses run | verdict |
|---|---|---|
| fence for the clone window only | 4/4 | 4 fatal |
| fence for the duration of the scrub | 4/4 | 4 fatal |
| fence until the record is retired | 4/4 | 4 fatal |
| make the RMW itself safe (no fence) | 1/4 | incomplete |
| freeze the stripe's free space | 4/4 | 4 fatal |
| opt-in forensic fence | **0/4** | **NOT REVIEWED** |
| evacuate the neighbours | **0/4** | **NOT REVIEWED** |
| stream during scrub, protect nothing | **0/4** | **NOT REVIEWED** |

The two angles scored `0 fatal` are **not** survivors. Every one of their
reviewers failed to run. Do not read those as passing.

### The one claim that was tested and refuted

The hypothesis was that protection could be free: since RAID5/6 forces CoW
(`can_nocow_file_extent()`, inode.c ~1913, gating both BTRFS_ORDERED_NOCOW and
BTRFS_ORDERED_PREALLOC), a committed extent is never overwritten in place, so
the only way a committed stripe's content changes is a new allocation into its
free space -- and removing that free space from the allocator would freeze the
stripe with no fence, no refused write and no bystander held hostage.

**Refuted.** `btrfs_repair_io_failure()` (bio.c:997) writes into a committed
stripe with no allocation, no ordered extent, and without entering raid56.c at
all. `btrfs_map_repair_block()` (volumes.c:9053) collapses the RAID56 write to
a single device write -- its own comment says "Not update any other mirrors nor
go through RMW path" -- so it writes one data column and does not recompute the
parity. Its only guard is `sb_rdonly()`.

So a freeze-only design has a hole, and would need a hold predicate in the
repair path as a companion. That companion costs no availability: it only
suppresses an opportunistic repair.

### What was claimed but is NOT established

The proposal's own weakest-point section argued this path is the *most likely*
destroyer of an ambiguous stripe's evidence, and that because it writes a
column without recomputing parity it turns "data stale, truth survives in the
parity" into truth nowhere -- with the sharp edge that the helper's own clone
pass is the read that triggers it.

Worked through on paper, that does not follow. RESOLVED by taking it per
VERTICAL stripe, which is the piece missing from the first pass: a wib bit
covers a whole 64KiB column, but the parity equation and the repair write-back
are both per sector row.

`repair_read_is_reconstruction()` (bio.c:186) is true for any mirror > 1 on
RAID5/6, and the guard at bio.c:274 declines only `BTRFS_CSUM_NONE`, so the
write-back does fire for a checksummed block that reconstructed cleanly. Two
cases, neither harmful:

- The row where the column is stale. The reconstruction of a checksummed
  neighbour consumes that stale column, so it does not match the true value and
  its checksum fails. No write-back, and the parity-implied value of the stale
  column survives untouched.
- Any other row. The reconstruction is correct and the write-back fires, but
  the parity for that row was computed by the filesystem from the correct
  sector and the corruption happened afterwards -- so restoring it makes the
  row consistent again rather than breaking it, and that sector does not appear
  in the equation for the stale row.

So the claim that this path is the likeliest destroyer of the evidence, and
that the helper's own clone pass triggers it, is **not supported**. A
measurement would still beat an argument and this series has been wrong by
reasoning before, so instrument it if the repair path is ever touched -- but
nothing should be designed around it.

What survives is the narrower point, and it is enough on its own: this is a
writer into a committed stripe that takes NO allocation, so a freeze keyed on
the allocator is incomplete whether or not this particular path is dangerous.

### Other findings worth keeping (self-reported, spot-checked, not all verified)

- A fence keyed on `full_stripe_logical` is satisfied vacuously by
  `btrfs_remove_chunk()` (volumes.c:3518), which deletes the mapping rather
  than writing -- reached from balance, the unused-bg cleaner (block-group.c:1818)
  and bg reclaim (block-group.c:2049). Verified separately this session that
  nothing in block-group.c calls any `btrfs_wib_*`; that is now fixed by
  `btrfs_wib_forget_range()`, but a *fence* would have the same blind spot.
- One refused write fails an entire ordered extent, up to BTRFS_MAX_EXTENT_SIZE
  (128MiB, fs.h:74), across potentially many inodes -- so a fence's blast radius
  is not bounded by the fenced stripe's size.
- `mark_stale_sectors()`'s budget bail fires on transients (a flapping cable, a
  controller reset), so any "fence until retired" design quarantines stripes
  that were never permanently ambiguous, with no automatic release.
- Automatic reclaim selects on `bg->used` alone (`should_reclaim_block_group()`,
  block-group.c:1919; `do_reclaim_sweep()`, space-info.c:2156), so freezing free
  space does not stop the reclaim worker handing the whole chunk to
  `btrfs_relocate_chunk()` with no user action.
- The evacuate design depends on REMAP_TREE, which is in
  `BTRFS_FEATURE_INCOMPAT_SUPP` only under CONFIG_BTRFS_EXPERIMENTAL
  (fs.h:334-347) and is mkfs-time. It cannot help a filesystem that develops an
  ambiguous stripe tomorrow.
- There is no REQ_FAILFAST anywhere in fs/btrfs and no btrfs-level IO timeout.
  An AMBIGUOUS stripe is by definition one with a column on a member whose last
  write did not land, so any design that reads it holds its lock across exactly
  the reads that cost tens of seconds.

### The decision this leaves

Not "which design", but whether to re-run the missing adversarial passes at all.
Three of the four angles the user's own answers pointed at were never reviewed.



---

## 16. The scrub write-back guard has no positive control

`scrub_stripe_read_repair_worker()` now refuses to write back a RAID5/6
reconstruction of a sector that has no checksum, is not metadata, and is not on
a stripe the write-intent log authorised (`stripe->wib_rebuild`).

Why it is needed: `scrub_verify_one_sector()` clears the error bit of an
unchecksummed sector unconditionally -- "we have no other choice but to trust
it" -- so a reconstruction from a stale parity is declared good, enters
`repaired`, and is written over the sector it was meant to fix. The read path
already refuses exactly this (`repair_read_is_reconstruction()`, bio.c:274);
scrub did the opposite. The guard that existed, `scrub_mark_wib_stale_sectors()`,
is gated on `wib_rebuild`, which is set only in the PROVEN branch of the parity
pass -- so the data pass, where RAID5/6 columns are scrubbed by
`scrub_simple_mirror()` during a different (often another device's) iteration,
had no protection at all.

**What is verified:** no regression. In-kernel self tests clean, and
`nocow_persist.sh` PASSes all three arms -- control destroys 8 of 32, the
persisted arm destroys 0 with every record retired, the ambiguous arm loses
nothing and keeps its records. The PROVEN repair path still repairs, which was
the risk.

**RESOLVED.** tools/testing/btrfs/uml/unprovable.sh measures it, with a working
negative control and a separately asserted precondition:

    precondition (device 2 omitted, forced through the parity)  16 wrong
    unchecksummed victim, guard in place                         0 destroyed
    unchecksummed victim, raid56_scrub_trusts_rebuild=1         16 destroyed
    checksummed victim, guard off                                0 destroyed

The third arm also settles the disagreement recorded in item 15 about whether
CHECKSUMMED sectors reconstructed from a divergent parity are written back.
They are not: that arm's precondition reports UNPROV_DIAG_UNREAD=144, i.e. when
those sectors are forced through the divergent parity the reconstruction fails
its stored checksum and the read ERRORS rather than returning a wrong value, so
nothing ever considers persisting it. Measured, not argued.

**Superseded, kept for the record:** that the guard catches anything. Grepping every
scenario log shows its warning never fires, so no existing test reaches the
path. The fix is conservative -- it only ever declines a write -- so shipping it
unproven is safe in the sense that the worst case is a repair not attempted.
But it is the second claim in this series argued rather than measured.

**The scenario that would close it** needs three things at once, and no current
harness produces them together:
- a data extent with no checksum (pre-existing NODATACOW, since `chattr +C` is
  now refused on RAID5/6),
- a genuinely divergent parity, which `nocow_persist.sh`'s prep arm already
  builds via failed writes -- note `raid56_stale_fake_bad_parity=1` only
  *records* the parity as bad, it does not make it arithmetically wrong,
- a READ error on that sector during an ordinary scrub's data pass, so the
  mirror loop reconstructs it. dm-error or dm-flakey on the right device at the
  right offset.

Then: without the guard the block must come back wrong after the scrub, and
with it the block must be reported unrepaired and left alone.


---

## 17. The evidence channel captures only on the user-scrub path

BTRFS_IOC_RAID56_EVIDENCE streams the data columns of an ambiguous full stripe
out during a scrub. The capture is wired into scrub_raid56_parity_stripe()'s
AMBIGUOUS branch only.

**CORRECTION, and the original claim here was wrong.** Mount-time recovery does
NOT bypass the capture. btrfs_scrub_raid56_full_stripe() calls
scrub_raid56_parity_stripe() (scrub.c:3806), the same function the user scrub
calls (scrub.c:3048), so the capture in its AMBIGUOUS branch fires on both
paths. The 3 a.m. case was covered from the first commit; the changelog that
said otherwise was mistaken.

It is also the one place the block group is NOT held read-only.
btrfs_inc_block_group_ro() appears only at scrub.c:3248, on the user-scrub
enumerate path; the recovery entry takes a block group reference and no RO. So
the coherence argument that makes the user-scrub capture sound -- nothing can be
writing while we copy -- does not hold there, and wiring the capture in
unchanged would hand out a snapshot that may not correspond to any instant.

**RESOLVED** by labelling rather than by holding. The capture sets
BTRFS_RAID56_EVIDENCE_F_COHERENT only when bg->ro is held, so a helper can tell
a coherent snapshot from a possibly-torn one, and evidence.c prints
"coherent NO -- captured by mount recovery without a block group hold".

Taking the RO in the recovery path was rejected: btrfs_inc_block_group_ro()
returns -ENOSPC when it cannot reserve elsewhere, and a mount recovering a
degraded array is precisely where that fails. Refusing to recover in order to
protect evidence would trade the filesystem for the evidence. Preserve, label,
never block.

**Smaller things also open:**
- No test exercises the ring's drop paths (dropped_full, dropped_wide). The
  counters exist and are reported; nothing proves they are right.
- Stripes wider than 16 columns are named but not copied
  (BTRFS_RAID56_EVIDENCE_MAX_BYTES). Untested; no such array was built.
- The helper must pread the parity itself. That is sound only while the block
  group is read-only, i.e. only during that chunk's scrub. A helper that drains
  the queue and reads the parity later races relocation and the allocator.
  Nothing enforces the ordering, and nothing warns.


---

## 18. RESOLVED: the unverified rebuilt sectors are free space

The counters showed 16-32 sectors per degraded boot rebuilt and returned with
no checksum bit. Resolved: those addresses are not owned by anything.
`btrfs inspect-internal logical-resolve` returns ENOENT for every one of them,
and -- the part that makes that answer worth anything -- resolving a KNOWN
file's extent on the SAME degraded read-only mount succeeds
(`inode 257 offset 0 root 5`). So the resolver works there and the negative is
real: they are free-space sectors of the same stripe, which have no checksum
for the honest reason that there is nothing there to checksum.

Benign. The counter's "in the caller's bio" test is not a tight enough proxy
for "delivered as file content".

---

## 19. The RAID5/6 read path is closed; the residual is bounded OUTSIDE it

A degraded read used to reconstruct and return data with verification never
armed (`fill_data_csums()` had one caller, the RMW *write* path). Fixed. The
read path is now audited at the point of delivery and the audit is clean:

    delivered_unchecked      0    sectors with a checksum that nothing compared
    delivered_nocsum         0    sectors delivered with no checksum at all
    delivered_audit_skipped  0    rbios the audit could not examine

The third counter exists because the first two are only meaningful if the audit
actually ran; without it a zero could not be told from never looking.

**So the residual is not in this path.** A few files per run (0-4) still read
back complete, at the right length, with content never written and no error --
but with all three counters at zero, the RAID5/6 recovery delivered nothing
unverified, nothing unchecksummed, and nothing unexamined.

### The signature, which is the lead

Logging the actual bytes rather than digests shows the corruption is NOT
whole-file. The head of the file reads as zeros while the tail is intact:

    bg1-6  head=00000000000000000000000000000000  tail=029ecc...  (tail correct)
    bg2-7  head=05000000000000000000000000000000  tail=9e1c26...  (tail correct)

Per-sector mapping shows at least one case where exactly one 4 KiB sector in
the middle of the file is entirely zero (`bg1-5 sectors=12
all_zero_sectors=[1]`), with the rest of the file correct. Others are partially
corrupt within a sector.

A sector reading as zeros with no error is what a HOLE reads as -- legitimately,
with no checksum consulted. That is the thread to pull: whether the affected
range still has an extent covering it in the degraded mount, not whether the
RAID5/6 code reconstructed it correctly.

### The zeros are written by btrfs_data_csum_check()

Found, and it is not a mystery any more. `btrfs_data_csum_check()`
(fs/btrfs/inode.c) ends its mismatch path with:

    zeroit:
            btrfs_print_data_csum_error(...);
            for (int i = 0; i < nr_steps; i++)
                    memzero_page(phys_to_page(paddrs[i]), ...);

On a checksum mismatch it ZEROES the caller's page, deliberately, so a partial
read cannot expose stale bytes. `repair_one_sector()` then reads the repair
directly back into those same pages (`bio_add_page(repair_bio,
phys_to_page(paddrs[i]), ...)`). So the zeros are already accounted for: any
sector that fails its checksum is zeroed, and it stays zeroed unless a repair
puts something else there.

That means the remaining question is not "where do zeros come from" but "how
does a zeroed sector reach the reader with the bio still reporting success",
since every path that should catch it sets BLK_STS_IOERR:
`repair_one_sector()` when num_copies == 1, and `btrfs_end_repair_bio()` when
the mirrors are exhausted.

Measured outcomes of the repair path on runs that reproduce:

    repair_csum_ok            32-48
    repair_csum_mismatch      20-27
    repair_csum_none          0
    repair_csum_none_raid56   0

So repairs are succeeding WITH a verified checksum, which means the page they
left behind matched its checksum and is correct. A sector that is both
correct-by-checksum and zero is a contradiction. The `unrestored` guard above
then ruled out the remaining possibility on that side: no sector is zeroed and
left unrepaired either. So `zeroit` does not produce the zeros that reach the
reader, and the search moves off the repair path entirely.

### A second fix: kept, but it does not close this bug

A zeroed sector must never be delivered as success. `btrfs_failed_bio` now
carries `unrestored`: incremented when a repair is started, decremented by
every path that accepts or fails it, and checked when the last repair
completes. Anything still outstanding means a sector was zeroed by the failed
checksum check and no repair put it back, so the read fails instead of
returning zeros.

It is safe where the previous attempt was not, because `zeroit` only runs when
a checksum EXISTS to mismatch -- a nodatacow read never increments the counter,
so degraded reads of unchecksummed data are untouched.

**It never fires on the runs that reproduce the symptom.** So no sector is
zeroed-and-unrestored, and `zeroit` is NOT the source of these particular
zeros. Kept anyway as an invariant with no measured cost: it makes
"zeroed and silently returned" unreachable by construction, which is worth
having whether or not it is this bug.

### A fix that was tried, measured, and reverted

Making the unverifiable-reconstruction branch of `btrfs_end_repair_bio()` fail
the read (`bi_status = BLK_STS_IOERR`) instead of returning the content is
policy-conformant on its face -- a reconstruction nothing can check is a guess,
and the series refuses to persist those already.

It is wrong, and the suite caught it. It makes EVERY degraded read of
unchecksummed data fail, including the ones whose reconstruction is perfectly
good, taking nodatacow files on a degraded array from readable to unreadable.
`nocow_persist.sh`'s control arm drops from 8 damaged blocks to 0 -- not
because nothing was damaged, but because nothing could be read at all.

Reverted, with the asymmetry now stated in the code: an unverifiable
reconstruction is good enough to hand to a caller who asked for it, and not
good enough to write over the only other copy. Persisting is the irreversible
half.

### Where the zeros are NOT, by measurement

Every path that can make a btrfs data read return bytes it did not read was
counted, on runs that reproduce the symptom. All of them are zero:

    delivered_zero           0   raid56 hands back no zero sector
    delivered_unchecked      0   nothing with a checksum went uncompared
    delivered_nocsum         0   nothing without a checksum was delivered
    delivered_audit_skipped  0   and the audit actually ran
    read_hole_true           0   no block served as a real hole
    read_hole_prealloc       0   no block served as a prealloc hole
    read_past_eof            0   no block zeroed as beyond last_byte
    read_already_uptodate    0   no block skipped as already uptodate

(The four read_* counters were temporary: they are per-block atomics in the
generic buffered-read loop, which is not a cost to leave in for every btrfs
user once they have answered. They answered zero and were removed. The four
delivered_* counters are in the RAID5/6 path only and stay.)

So the zeros enter AFTER btrfs_do_readpage() builds and submits the bio, and
AFTER raid56 hands back non-zero content -- which leaves the bio completion and
read-repair machinery in fs/btrfs/bio.c (btrfs_end_repair_bio(),
next_repair_mirror(), the bio_reset/retry into the original pages). That is
where the next person should instrument, and it is a small amount of code.

### The zeros do not come from raid56

`delivered_zero` counts data sectors a degraded read hands back that are
entirely zero, measured in the rbio at delivery. It is **0** on runs that
produce files with contiguous runs of zero sectors:

    dz1  delivered_zero 0    bg1-6 all_zero_sectors=[1 2 3 4]
                             bg3-7 all_zero_sectors=[3 4 5]
    dz2  delivered_zero 0    bg2-5 all_zero_sectors=[7 8 9]

So raid56 delivers correct bytes and something ABOVE it replaces them with
zeros. The runs are 3-4 sectors, i.e. 12-16 KiB, not single sectors and not
whole files.

That, plus the RAID1 control below, is the whole shape of the remaining defect:
a btrfs read path above raid56 zero-fills a 12-16 KiB run of an extent-backed,
checksummed file on a degraded RAID5/6 mount, without an error and without the
checksum being consulted.

btrfs zero-fills a data range when the extent map says hole
(`btrfs_do_readpage()`), and a hole is not checksum-verified because there is
nothing there to verify. A range that is BOTH covered by an extent with a
checksum item AND served as a hole is a contradiction, and that is the
invariant worth enforcing: if a checksum item exists for a range, that range
must never be served as zeros. That check is cheap to state and expensive to
place -- it belongs in the generic read path, not in raid56, and it must not
break genuinely sparse files.

**Why this was not patched here.** The remaining fix is in the generic btrfs
read path, which every profile and every workload uses. This series has been
wrong by reasoning eleven times on this one bug; shipping a speculative change
to that path on the strength of a twelfth hypothesis would risk a worse defect
than the one being fixed, in code far outside the RAID5/6 subsystem this work
was scoped to. The scoping above is the deliverable: a reproducer, a control
that excludes every other profile, a counter that excludes raid56 as the
source, and a named invariant to enforce.

### The two facts that bound it

**It is extent-backed and checksummed, and still reads as zeros.** For
`bg2-6`: `filefrag` shows ONE extent covering blocks 0..14 with no hole, and
`BTRFS_IOC_GET_CSUMS` shows `off=0 len=61440 HAS_CSUMS`. Sector 5 of that file
reads back entirely zero, with no error. So it is not a hole, and a checksum
exists that nothing rejected it against.

**It is RAID5/6-specific.** The same scenario with a RAID1 data profile
(`dmfail34.sh <kernel> tag flakey raid1:raid1 rw 4 2`) produces 7 honest read
failures and ZERO silent corruption across four degraded boots. RAID5/6
produces silent corruption; RAID1 does not.

Together with the clean delivery audit, that places the defect in RAID5/6 read
handling OUTSIDE reconstruction -- the recovery delivers nothing unverified,
yet only RAID5/6 corrupts. Worth looking at the sector/step/folio arithmetic
that is specific to raid56 (`sector_nsteps`, `btrfs_bio_for_each_block_all`,
the partial-folio copies), where a sector could be left zero-filled after
verification rather than before it.

### Eliminated, each with evidence

Manifest duplicates; wrong expectation (source digest recorded alongside the
read-back, they agree every time); whole-file zeros; another tracked file's
content; NODATACOW; unverified rebuilds (those addresses are free space, with a
working positive control on the resolver); degraded read-write mounts mutating
the array between boots (`DEGRADED_MOUNT=ro`, symptom survives); RAID1 metadata
divergence (per-boot extent map identical in every boot); verification reading
different memory than is delivered (both use `sector_paddr_in_rbio(..., 0)`).

Ten hypotheses have died. The next step is not an eleventh -- it is to check
whether the file's extent still covers the zeroed sector on the degraded mount.

Reproduce: `DEGRADED_MOUNT=ro BTRFS_TEST_DIR=... tools/testing/btrfs/uml/dmfail34.sh
<kernel> tag flakey raid5:raid1 rw 4 2`, then read `_BADZERO`, `_EXTENT`,
`_BAD`, `_RECOVER` from `results.<tag>`.

## 20. RESOLVED: the residual was a split bio's error being dropped

Section 19 is right that the RAID5/6 read path delivered nothing unverified,
and right that the residual is outside it. It is in `btrfs_bio_end_io()`, one
level up, and the framing that kept it hidden for so long was "where do the
zeros come from". The zeros were a subset of the damage, not the damage.

**The measurement that turned it.** `csummap` was extended to print, per
sector, the checksum the csum tree holds next to the crc32c of the bytes the
filesystem returns. For every affected file:

    CSUMSEC bg2-5 sec=0  stored=02fe82b4 actual=46127271 MISMATCH
    ...
    CSUMSEC bg2-5 sec=8  stored=8ab3b38a actual=98f94189 MISMATCH   (zeros)
    CSUMSEC bg2-5 sec=9  stored=51774fce actual=98f94189 MISMATCH   (zeros)
    CSUMSEC bg2-5 sec=10 stored=cc8721c7 actual=cc8721c7 MATCH
    CSUMMAP ... mismatch=10 csum_holes=0 stored_is_zeros=0

So the checksums are correct, the delivered bytes are wrong, most of the wrong
bytes are not zeros at all, and nothing objected. Not "a hole read as zeros",
not "a zero page was checksummed", not "no checksum covers this": the read path
returned data it did not verify.

**The footprint.** Every corrupted run ends exactly at a 64 KiB stripe-column
boundary and covers precisely the columns on the missing device; sectors past
the boundary read back correct. That held for all 19 occurrences collected
before the cause was known, and 64 KiB is `BTRFS_STRIPE_LEN` -- the point at
which `btrfs_submit_chunk()` splits a bio.

**The cause.** A split read becomes a clone for the first half and the
original, advanced, for the rest. `btrfs_bio_end_io()` keeps the first error in
`bbio->status` and copied it into `bbio->bio.bi_status` only when the half
finishing LAST had succeeded. On a degraded array the present half is read
straight off a device while the missing half goes through
`raid56_parity_recover()` on a workqueue, so the failing half routinely
finishes last -- and by then `bbio->bio.bi_status` was already `BLK_STS_OK`.
`end_bbio_data_read()` reads one status for the whole original bio and marks
every folio uptodate on it, so the failed half's folios -- holding the
reconstruction the checksum had already rejected -- were handed back as the
file's content.

Writes had the same hole: a write whose failing half finished last was reported
as having succeeded.

**Fixed** by loading `bbio->status` unconditionally; it is `BLK_STS_OK` until
some half fails and holds the first error after that.

**Measured**, `tools/testing/btrfs/uml/split_status.sh`, three rounds summed
across every degraded boot, with `split_bio_status_legacy=1` as the negative
control:

    control: 29 files damaged, 8 of them returned silently
    fixed  : 30 files damaged, 0 of them returned silently

Both arms still detect the damage. Only the silent delivery changes -- which is
the distinction `verify_manifest()` now counts separately, because summing a
read that fails together with a read that lies had been hiding exactly this.

The regression suite passes end to end with the fix, and `regress.sh
--check-log` re-judges the two arms of `split_status.sh` directly: the
legacy-mode run fails ("2 file(s) read back complete with different content"),
the fixed run passes with its 10 unreadable files noted as the expected
residual. So the suite detects this defect from an ordinary flakey run, not
only from the dedicated scenario.

Fixing the kernel exposed a contradiction in the suite itself: `check_scenario()`
split a bad file into "read failed" (expected) and "read back wrong" (the
defect), then failed on `bad=` -- which is both counts summed. It had never
been reached because the flakey scenario always tripped the "read back wrong"
branch first and returned. `bad=` is now a cross-check that the `_BAD` line
parsing accounts for every bad file, and the verdict moved to the scenario's
own `silent=` count.

**How far it reaches.** The lost status is `bbio->bio.bi_status`, which is
what every `end_io` callback reads to decide whether its I/O succeeded, so the
same hole was open to all of them -- buffered data reads
(`end_bbio_data_read()`), metadata reads, compressed I/O
(`compression.c:234`), direct and encoded I/O (`btrfs_encoded_read_endio()`),
relocation (`relocation.c:4095`) and scrub (`scrub_read_endio()`). Each takes
its answer from the last completion to run, and until now so did the status.
Only the degraded buffered read was measured; the rest is a code fact about a
shared field, and every one of them is fixed by the same load. Which of them
can actually be reached with a split bio was not measured -- scrub, for one,
submits stripe-aligned reads of at most `BTRFS_STRIPE_LEN` and so may never
split.

Every path that writes `bbio->bio.bi_status` either passes the value straight
into `btrfs_bio_end_io()` (`simple_end_io_work()`, `btrfs_raid56_end_io()`,
`orig_write_end_io_work()`, `run_one_async_done()`, `btrfs_repair_done()`) or
clears it before completion as `btrfs_check_read_bio()` does, so loading
`bbio->status` unconditionally cannot invent an error. Repair bios never reach
`btrfs_bio_end_io()` at all -- `btrfs_check_read_bio()` diverts the repair
bioset to `btrfs_end_repair_bio()` first -- so a repair retried against another
mirror carries no stale status into its second attempt.

**What this retires.** The `unrestored` guard in `struct btrfs_failed_bio`
never fired on any reproducing run and still does not; it stays as an invariant
with no measured cost, but it was never this bug. The open question at the end
of section 19 -- whether the extent still covers the zeroed sector -- is moot:
it does, and the sector was reconstructed, checked, rejected, and returned
anyway.

---

## 21. A device that comes back restores nothing until a mount or a scrub

**What.** Recovery runs from **three** places, not one -- this entry said "at
mount and nowhere else" when it was written and that was wrong:
`btrfs_wib_rw_mount()` at mount (`disk-io.c:3740`), the same function on a
**remount ro->rw** (`super.c:1334`), and `btrfs_wib_recover_after_replay()`
after a tree log replay (`disk-io.c:3788`). So a `mount -o remount,rw` closes
the window too, which is a materially cheaper answer than a full mount cycle
and should be said in any advice given to an operator.

Three things retire a record and restore a stripe's redundancy: recovery at
any of those three entry points, a device replace (which drives
`btrfs_scrub_dev()` over the source device's stripes, so the record's plan
applies there), and any `btrfs scrub` (`scrub.c:2811`).

A device that was missing and then reappears is not one of them. On a
long-lived mount the exposure window is unbounded: the data still reads
correctly -- the record is what makes a degraded read rebuild those sectors
rather than believe them -- but the redundancy of every stripe recorded against
that device stays gone until somebody mounts or scrubs.

This is the same window item 1 is about, reached from the other direction.

**Preliminary, needs confirming.** There may be nothing to hook. The two places
that clear `BTRFS_DEV_STATE_MISSING` are `device_list_add()` (the scan path,
which operates on the *unmounted* device list) and `btrfs_close_one_device()`
(teardown). Neither is "this disk is back and usable while the filesystem is
mounted", and a missing device's `btrfs_device` has `bdev == NULL` with nothing
re-opening it. If that survives an exhaustive check, then "hook the reconnect"
is not implementable as stated and the question becomes what to do instead.

**Options, none chosen.**

(a) *A background retire pass* -- scrub recorded stripes on a timer or at idle,
    without waiting for a mount or a device event. **Proposed and declined**
    pending research: it spends IO nobody asked for, its interaction with a
    still-degraded array is unexamined, and it answers a question adjacent to
    the one actually asked rather than the one asked. Recorded here so it is
    not re-proposed as obvious.
(b) *Make a reconnect path exist*, then hook it: allow a missing device to be
    re-opened on a mounted filesystem and trigger recovery for the stripes
    recorded against it. Much larger: touches device lifetime, the device list
    mutex, and every assumption that `bdev == NULL` means gone for this mount.
(c) *Surface and require an explicit scrub.* The exposure is already
    enumerable through `BTRFS_IOC_RAID56_STALE_STRIPES` and the sysfs counters.
    Document that a returning device needs `btrfs scrub` and make the state
    visible enough that a monitoring system can see it.
(d) *Rely on the paths that exist.* A replace already covers it. Argue that a
    device which genuinely left should be replaced, not re-admitted.

**What research would settle it.** Whether a missing device can rejoin a
mounted btrfs at all, exhaustively rather than by reading two call sites; what
MD does when a member rejoins an array and whether that maps; and how long the
window actually is in practice, which needs the exposure measured on a mount
that survives a device going and coming back rather than modelled.
