"""Shared widen-only range update for observed.<target> blocks.

Both runners (scripts/scenario/run_scenario.py and run_live_scenario.py)
persist a per-target rolling [min, max] range for each scalar in the
observed block. The range expands when a new measurement falls outside
its current bounds; otherwise the JSON isn't rewritten — drastically
reducing diff churn from routine runs.

Shape:
    observed.<target> = {
        "tick_us":         [min, max],
        "free_heap":       [min, max],
        "max_alloc_block": [min, max],
        "at":              [first_seen_iso, last_updated_iso],
    }

Min/max is the literal numeric range. The "what to watch" mapping is a
property of the contract direction (tick contract = ceiling, so observed
max is the failure indicator; heap/block contract = floor, so observed
min is the failure indicator). See docs/testing.md § Persistent observations.

When the user explicitly renegotiates the contract (--update-contract),
the observed range resets to the current single-point measurement — the
historical range was for the *previous* contract and no longer applies.
That reset is the caller's responsibility (this module just widens or
seeds the range; the caller decides when to call which).
"""
from __future__ import annotations


_FIELDS = ("tick_us", "free_heap", "max_alloc_block")


def widen(existing: dict | None, sample: dict, today: str) -> tuple[dict, bool]:
    """Return (new_observed_block, changed) given an existing block (or None)
    and a fresh measurement sample {field: scalar, ...}.

    - If existing is None (first observation for this target), seed both ends
      of the range to the sample value and stamp `at = [today, today]`.
    - If the sample is inside the existing range for every field, return the
      existing block unchanged and changed=False — the runner can skip writing.
    - Otherwise, widen each field's range as needed and stamp `at = [first,
      today]` (keeping the original first_seen)."""
    if existing is None or not _is_range_shape(existing):
        block = {f: [int(sample[f]), int(sample[f])] for f in _FIELDS if f in sample}
        block["at"] = [today, today]
        return block, True

    new_block = dict(existing)
    changed = False
    for f in _FIELDS:
        if f not in sample:
            continue
        v = int(sample[f])
        cur = existing.get(f)
        if not isinstance(cur, list) or len(cur) != 2:
            new_block[f] = [v, v]
            changed = True
            continue
        lo, hi = int(cur[0]), int(cur[1])
        new_lo, new_hi = min(lo, v), max(hi, v)
        if new_lo != lo or new_hi != hi:
            new_block[f] = [new_lo, new_hi]
            changed = True

    if changed:
        at = existing.get("at")
        first = at[0] if isinstance(at, list) and len(at) == 2 else today
        new_block["at"] = [first, today]
    return new_block, changed


def reset(sample: dict, today: str) -> dict:
    """Build a fresh single-point observed block — called when the contract
    is renegotiated and the previous range's history no longer applies."""
    block = {f: [int(sample[f]), int(sample[f])] for f in _FIELDS if f in sample}
    block["at"] = [today, today]
    return block


def _is_range_shape(block: dict) -> bool:
    """True if at least one numeric field is already in [min,max] list form.
    Used to detect post-migration shape vs the older scalar shape."""
    for f in _FIELDS:
        v = block.get(f)
        if isinstance(v, list) and len(v) == 2:
            return True
    return False
