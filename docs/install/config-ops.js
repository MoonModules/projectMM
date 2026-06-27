// Plan the ordered APPLY_OP sequence that applies a device-model catalog entry's config.
// Pure (no I/O, no browser globals) so the install orchestrator can import it and a node
// unit test can exercise the ordering directly. The orchestrator sends each op this
// returns over serial (APPLY_OP) — or the HTTP path POSTs the equivalent.
//
// Op order: a clearChildren pre-pass, then per-module add, then per-control set.
//
// The clear pre-pass is the fix for "defaults only apply if I also erase the flash". On a
// NON-erased device the persisted tree already holds modules the entry re-adds (a prior
// flash's drivers under Drivers, Audio under System, …). The device-side add is
// idempotent-on-id (an existing name returns AlreadyExists and is skipped), so without
// clearing first a stale module lingers and a structural change never lands. So clear the
// user-editable children of every parent the entry adds into (plus any container flagged
// replaceChildren) BEFORE adding. The device's clearChildren preserves boot-wired children
// (Preview, Improv), so a parent's apparatus survives and only swappable pipeline content
// is replaced — the no-erase path then converges to the same tree an erase+flash produces.
// A module the entry adds: a non-empty id, a parent to add it under, and a type. The clear
// pre-pass and the add pass MUST agree on this (one helper, used by both) — otherwise a
// malformed module could get a clearChildren on its parent without a matching add.
function isAddable(m) {
    return !!(m && typeof m === "object" &&
              typeof m.id === "string" && m.id &&
              typeof m.parent_id === "string" && m.parent_id &&
              m.type);
}

export function planConfigOps(entry) {
    const ops = [];
    const modules = entry && Array.isArray(entry.modules) ? entry.modules : [];

    // Modules the entry adds fresh — no need to clear their children (a just-created
    // module has none), so a parent that is itself added is dropped from the clear-set.
    const addedIds = new Set(modules.filter(isAddable).map(m => m.id));

    const clearParents = new Set();
    for (const m of modules) {
        if (!m || typeof m !== "object") continue;
        if (m.replaceChildren && typeof m.id === "string" && m.id) clearParents.add(m.id);
        if (isAddable(m)) clearParents.add(m.parent_id);
    }
    for (const parent of clearParents) {
        if (addedIds.has(parent)) continue;   // freshly added → nothing to clear
        ops.push({ op: "clearChildren", parent });
    }

    for (const m of modules) {
        if (!m || typeof m !== "object" || typeof m.id !== "string" || m.id === "") continue;
        if (isAddable(m)) ops.push({ op: "add", type: m.type, id: m.id, parent: m.parent_id });
        const controls = m.controls;
        if (controls && typeof controls === "object") {
            for (const [control, value] of Object.entries(controls)) {
                ops.push({ op: "set", module: m.id, control, value });
            }
        }
    }
    return ops;
}
