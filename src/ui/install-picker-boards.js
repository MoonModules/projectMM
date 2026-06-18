// install-picker-boards.js — the board-catalog + chip-detection half of the
// release picker. WEB-INSTALLER ONLY. It is imported by the GitHub Pages
// installer (docs/install/index.html) and passed into installPicker.init() as
// the `boardSupport` option; the shared install-picker.js never imports it.
//
// Why a separate file: install-picker.js is embedded into the firmware binary
// (src/ui/embed_ui.cmake gzips it verbatim — there is no bundler or tree-shaking
// in this project, so whatever is in that file ships on the board). The board
// catalog and chip detection are only meaningful during a first USB flash from
// the browser; on-device OTA already knows its board (BoardModule). Keeping this
// code out of install-picker.js keeps it out of every device's flash. The
// injection seam (init({ boardSupport })) is the standard "host supplies the
// optional capability" pattern: the Pages page wires it in, the device passes
// nothing, so the board code is genuinely absent from the firmware.
//
// Pure DOM + a same-origin fetch — no serial / esptool / Improv (those live in
// install-orchestrator.js and reach this code only via the onDetect callback
// install-picker.js already owns).

// Boards catalog — same-origin docs/install/boards.json. ~1 KB, no rate-limit
// concern (CDN serves it on the public site, preview_installer serves it from
// disk locally), so no sessionStorage cache: caching adds invalidation bugs
// without saving bytes. Graceful degradation: any fetch / parse failure returns
// [] and the picker silently omits the board <select>.
export async function loadBoards() {
    try {
        const res = await fetch("./boards.json");
        if (!res.ok) return [];
        const data = await res.json();
        return Array.isArray(data) ? data : [];
    } catch (_) {
        return [];
    }
}

// Rebuild a board <select>: a leading pass-through option (label varies by
// context) followed by one option per board. Used by the picker's render()
// (full catalog) and applyDetectedChip() (chip-narrowed list) so the option-
// building shape lives in one place.
export function fillBoardOptions(boardEl, boards, passthroughLabel) {
    boardEl.replaceChildren();
    const any = document.createElement("option");
    any.value = "";
    any.textContent = passthroughLabel;
    boardEl.appendChild(any);
    for (const b of boards) {
        const opt = document.createElement("option");
        opt.value = b.name;
        opt.textContent = b.name;
        boardEl.appendChild(opt);
    }
}

// After a successful Detect, narrow the board <select> to ONLY the boards whose
// `chip` matches the detected family — the other family is removed from the list
// entirely (plug in an S3, the classic-ESP32 boards disappear, and vice versa).
// The pass-through is relabelled "Other / generic <chip>" so a user with a board
// not in the catalog can still flash the right firmware for their silicon. The
// returned status string is shown next to the Detect button.
// Detection gives chip FAMILY only — it can't tell esp32 / esp32-eth /
// esp32-eth-wifi apart (same silicon, different wiring), so when several boards
// share the family we narrow + let the user pick rather than guessing.
export function applyDetectedChip(state, boardEl) {
    const matches = state.boards.filter(b => b.chip === state.detectedChip);
    if (matches.length === 0) {
        // A chip we ship no board for: don't strand the user — leave the full
        // list and report it. (selectedBoard unchanged.)
        return `Detected ${state.detectedChip} — no matching board, pick manually`;
    }
    fillBoardOptions(boardEl, matches, `Other / generic ${state.detectedChip}`);
    let autoName = "";   // a board we auto-selected (single match, or a generic default)
    if (matches.length === 1) {
        state.selectedBoard = matches[0].name;   // exactly one board → auto-pick
        autoName = matches[0].name;
    } else if (!matches.find(b => b.name === state.selectedBoard)) {
        // Several boards in this family and no current pick in it (fresh detect, or a
        // prior pick from the other family). Prefer the catalog's generic board for
        // this chip if one exists (e.g. "Generic ESP32 Dev") — a sensible no-overrides
        // default; otherwise leave the "Other / generic <chip>" pass-through selected
        // (S3/P4 ship no generic entry) so we don't guess a specific board.
        const generic = matches.find(b => /generic/i.test(b.name));
        state.selectedBoard = generic ? generic.name : "";
        if (generic) autoName = generic.name;
    } else {
        autoName = state.selectedBoard;   // an in-family pick survives the detect
    }
    boardEl.value = state.selectedBoard || "";
    return autoName
        ? `Detected ${state.detectedChip} — selected ${autoName}`
        : `Detected ${state.detectedChip} — pick your board (${matches.length} match)`;
}
