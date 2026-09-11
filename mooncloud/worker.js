// SPDX-License-Identifier: GPL-3.0-or-later
//
// MoonCloud Stats: the whole server.
//
// Two endpoints and one table. POST /api/report takes one report from a device that consented;
// GET /api/stats hands back the aggregates, which is what the device's own card renders.
//
// Cloudflare Workers rather than a box we own, for one specific reason: `request.cf.country`
// resolves the country at the EDGE, so the privacy policy's promise that the IP address is never
// stored is structural. Nothing here ever sees an address to write down, where on our own server
// that promise would be one nginx config change away from being false.
//
// The source is public for the same reason the firmware is: "the deployed code is the published
// code" is checkable rather than asked for on trust.

// Every field a report may carry. Anything else a client sends is DROPPED rather than stored:
// the device is trusted to be honest, not to be current, and a future firmware inventing a field
// must not silently start filling a column nobody agreed to.
const ALLOWED = [
  "installationId",
  "event",
  "version",
  "previousVersion",
  "chip",
  "flash",
  "psram",
  "sdk",
  "deviceModel",
  "modules",
  "totalHeap",
  "freeHeap",
  "lightCount",
  "dev",
];

// What a value may look like. A report is untrusted input from an open endpoint, so the point is
// bounding what reaches storage rather than deciding what is true.
const MAX_STRING = 64;
const MAX_MODULES = 64;
const MAX_NUMBER = 2147483647;   // a device fact, not a size to trust: bounded like every string

function clean(report, country) {
  const row = {};
  for (const key of ALLOWED) {
    const value = report[key];
    if (value === undefined || value === null) continue;

    if (key === "modules") {
      if (!Array.isArray(value)) continue;
      row.modules = value
        .filter((m) => typeof m === "string" && m.length <= MAX_STRING)
        .slice(0, MAX_MODULES)
        .join(",");
      continue;
    }
    // `dev` is the one boolean in the report: whether this firmware came from a release or a
    // local build. Stored as 0/1 because the column is an INTEGER and SQLite has no bool.
    if (key === "dev") {
      row.dev = value === true || value === "true" ? 1 : 0;
      continue;
    }
    // The numeric fields arrive as JSON NUMBERS, so the string test below would drop them: every
    // other allowlisted field is text, and only `modules` and `dev` had branches of their own.
    // Bounded and floored at 0, because a report is untrusted input from an open endpoint.
    if (key === "totalHeap" || key === "freeHeap" || key === "lightCount") {
      const n = typeof value === "number" ? value : Number(value);
      row[key] = Number.isFinite(n) && n > 0 ? Math.min(Math.floor(n), MAX_NUMBER) : 0;
      continue;
    }
    if (typeof value !== "string") continue;
    if (value.length > MAX_STRING) continue;
    row[key] = value;
  }

  // Derived here, never sent by the device and never stored alongside an address.
  row.country = country || "??";
  row.receivedAt = new Date().toISOString().slice(0, 10); // the DAY, not the moment: a timestamp
                                                          // precise to the second is a fingerprint
                                                          // when combined with a country.
  return row;
}

async function handleReport(request, env) {
  let report;
  try {
    report = await request.json();
  } catch {
    return json({ error: "not JSON" }, 400);
  }
  if (typeof report !== "object" || report === null) {
    return json({ error: "not an object" }, 400);
  }
  // An id is what makes a row countable as an installation rather than an event. A report without
  // one is accepted and discarded: rejecting it would tell a caller what we require, and the
  // device never sends one without consent anyway.
  // Hex, not merely 32 characters: an id is a truncated SHA-256, and anything else did not come
  // from a device. Accepted-and-discarded rather than rejected, so a caller learns nothing about
  // what is required.
  if (typeof report.installationId !== "string" || !/^[0-9a-f]{32}$/.test(report.installationId)) {
    return json({ ok: true }, 202);
  }
  // `event` reaches every user's card as a pie legend, so it is one of two words or nothing.
  if (report.event !== undefined && report.event !== "install" && report.event !== "upgrade") {
    return json({ ok: true }, 202);
  }

  const row = clean(report, request.cf?.country);

  // One row per installation per version: re-reporting the same upgrade overwrites rather than
  // double-counting, which is what makes the totals a count of installations.
  await env.DB.prepare(
    `INSERT INTO reports
       (installationId, event, version, previousVersion, chip, flash, psram, sdk, deviceModel, modules, country, receivedAt, totalHeap, freeHeap, lightCount, dev)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(installationId, version) DO UPDATE SET
       event = excluded.event,
       previousVersion = excluded.previousVersion,
       chip = excluded.chip,
       flash = excluded.flash,
       psram = excluded.psram,
       sdk = excluded.sdk,
       deviceModel = excluded.deviceModel,
       modules = excluded.modules,
       country = excluded.country,
       receivedAt = excluded.receivedAt,
       totalHeap = excluded.totalHeap,
       freeHeap = excluded.freeHeap,
       lightCount = excluded.lightCount,
       dev = excluded.dev`
  )
    .bind(
      row.installationId,
      row.event ?? "install",
      row.version ?? "",
      row.previousVersion ?? "",
      row.chip ?? "",
      row.flash ?? "",
      row.psram ?? "",
      row.sdk ?? "",
      row.deviceModel ?? "",
      row.modules ?? "",
      row.country,
      row.receivedAt,
      row.totalHeap ?? 0,
      row.freeHeap ?? 0,
      row.lightCount ?? 0,
      row.dev ?? 0
    )
    .run();

  // The same report appended to the history table, which is never updated: `reports` above holds
  // current state and overwrites on conflict, so without this the day a device FIRST reported is
  // lost the moment it reports again, and a trend added later would start from empty. Carries no
  // installation id: a dated row per installation is a movement profile, and counting events needs
  // no identity.
  await env.DB.prepare(
    `INSERT INTO events (event, version, previousVersion, chip, deviceModel, country, day, dev)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  )
    .bind(row.event ?? "install", row.version ?? "", row.previousVersion ?? "",
          row.chip ?? "", row.deviceModel ?? "", row.country, row.receivedAt, row.dev ?? 0)
    .run();

  return json({ ok: true });
}

// The aggregates, and ONLY aggregates: this endpoint is readable by anyone, so it may never carry
// a row. Counts of chips and versions are safe to show the world; an installation id is not.
async function handleStats(env, url) {
  // EVERY figure counts every report. Nothing is filtered out here, and the `Build` breakdown below
  // is what shows the released-against-development split, so a reader sees the distinction rather
  // than a total that quietly left one side out.
  //
  // The earlier shape excluded development installs from every other figure by default. It made the
  // headline number smaller with nothing saying why, and it hid a developer's own bench boards from
  // the card running ON one of them, which reads as a report that never arrived.
  //
  // A caller narrows any dimension by naming it: `?chip=ESP32`, `?country=NL`, `?dev=0`. Every chart
  // then re-counts within that population, which is what makes a slice clickable in the UI.
  //
  // The filter is a CHOICE, never a default: the charts count everything until a reader asks
  // otherwise, so a narrowed total is always something they did rather than something hidden.
  //
  // Column names come from this table, never from the query string, so a parameter cannot reach the
  // SQL. Values are bound.
  const FILTERABLE = ["version", "previousVersion", "chip", "flash", "psram", "sdk",
                      "deviceModel", "country", "event"];
  const where = [];
  const binds = [];
  for (const column of FILTERABLE) {
    const value = url?.searchParams.get(column);
    if (value === null || value === undefined || value === "") continue;
    where.push(`${column} = ?`);
    binds.push(value);
  }
  // `dev` is an integer flag rather than a string column, and `?dev=0` predates the general form.
  const releasedOnly = url?.searchParams.get("dev") === "0";
  if (releasedOnly) where.push("dev = 0");
  else if (url?.searchParams.get("dev") === "1") where.push("dev = 1");
  // One filter per role, matching the charts: `?driver=Preview`, `?effect=Lissajous`. Entries are
  // stored as `role:name` in one comma-joined column, so membership is a LIKE against the delimited
  // form rather than equality. Role names come from this table, never the query string; values are
  // bound.
  for (const role of ["driver", "service", "layout", "effect", "modifier"]) {
    const value = url?.searchParams.get(role);
    if (!value) continue;
    where.push("(',' || modules || ',') LIKE ?");
    binds.push(`%,${role}:${value},%`);
  }
  // A bucketed chart filters by BOUNDS: its slices name ranges ("64-128 KB"), and the column holds
  // the raw number, so there is no value to match on. `?freeHeapMin=65536&freeHeapMax=131072`.
  for (const column of ["totalHeap", "freeHeap", "lightCount"]) {
    const min = Number(url?.searchParams.get(`${column}Min`));
    const max = Number(url?.searchParams.get(`${column}Max`));
    if (Number.isFinite(min) && min > 0) { where.push(`${column} >= ?`); binds.push(min); }
    if (Number.isFinite(max) && max > 0) { where.push(`${column} <= ?`); binds.push(max); }
  }

  const filter = where.length ? ` AND ${where.join(" AND ")}` : "";
  const filtered = where.length > 0;

  const counts = async (column) => {
    const { results } = await env.DB.prepare(
      `SELECT ${column} AS name, COUNT(DISTINCT installationId) AS count
         FROM reports WHERE ${column} != ''${filter} GROUP BY ${column} ORDER BY count DESC LIMIT 40`
    ).bind(...binds).all();
    return results ?? [];
  };

  // `modules` is stored as one comma-separated string per report, so counting it needs the rows
  // rather than a GROUP BY: a module is present on an installation, and an installation appears
  // once. Counted over DISTINCT installations for the same reason every other figure is, so a
  // device that reported twice does not count twice.
  // Modules arrive as `role:name` (driver:Preview, effect:Lissajous), so one column yields a chart
  // per kind: which drivers and services are actually used is a different question from which
  // effects are, and one combined pie buried the first under the second.
  //
  // Counted over DISTINCT installations, like every other figure: a device that reported twice does
  // not count twice.
  const moduleCountsByRole = async () => {
    const { results } = await env.DB.prepare(
      `SELECT DISTINCT installationId, modules FROM reports WHERE modules != ''${filter}`
    ).bind(...binds).all();

    const byRole = new Map();   // role -> Map(name -> Set of installation ids)
    for (const row of results ?? []) {
      for (const entry of String(row.modules).split(",")) {
        const text = entry.trim();
        if (!text) continue;
        const cut = text.indexOf(":");
        // Every entry carries its role: a report predating the split cannot reach this, because the
        // tables are emptied before ship (ADR-0013: no migration code, documented breaks).
        if (cut <= 0) continue;
        const role = text.slice(0, cut);
        const name = text.slice(cut + 1);
        if (!name) continue;
        if (!byRole.has(role)) byRole.set(role, new Map());
        const names = byRole.get(role);
        if (!names.has(name)) names.set(name, new Set());
        names.get(name).add(row.installationId);
      }
    }

    const out = {};
    for (const [role, names] of byRole) {
      out[role] = [...names.entries()]
        .map(([name, ids]) => ({ name, count: ids.size }))
        .sort((a, b) => b.count - a.count)
        .slice(0, 40);
    }
    return out;
  };

  // Install versus upgrade. The one figure counted over REPORTS rather than installations: it
  // describes events, and an installation that upgraded three times is three upgrade events.
  const eventCounts = async () => {
    const { results } = await env.DB.prepare(
      `SELECT event AS name, COUNT(*) AS count FROM reports WHERE event != ''${filter}
        GROUP BY event ORDER BY count DESC`
    ).bind(...binds).all();
    return results ?? [];
  };

  // Released against development, and the ONE figure that ignores the dev filter: a pie whose job
  // is to show the split cannot be drawn from a query that already removed one side of it.
  const buildCounts = async () => {
    // Every filter EXCEPT dev: a pie whose job is to show the released-against-development split
    // cannot be drawn from a query that already removed one side of it. The other dimensions still
    // apply, so clicking `NL` narrows this pie to the Netherlands rather than leaving it global.
    const noDev = where.filter(c => c !== "dev = 0" && c !== "dev = 1");
    const devless = noDev.length ? ` WHERE ${noDev.join(" AND ")}` : "";
    const { results } = await env.DB.prepare(
      `SELECT CASE dev WHEN 1 THEN 'development' ELSE 'released' END AS name,
              COUNT(DISTINCT installationId) AS count
         FROM reports${devless} GROUP BY dev ORDER BY count DESC`
    ).bind(...binds).all();
    return results ?? [];
  };

  // Bucketed on READ rather than at the device, so a range that turns out wrong can be re-cut
  // against rows already stored. Raw values would give one slice per device and say nothing.
  const bucketed = async (column, buckets) => {
    const { results } = await env.DB.prepare(
      `SELECT DISTINCT installationId, ${column} AS v FROM reports WHERE ${column} > 0${filter}`
    ).bind(...binds).all();
    const counts = new Map();
    for (const row of results ?? []) {
      const label = buckets.find(b => row.v <= b.max)?.name ?? buckets[buckets.length - 1].name;
      counts.set(label, (counts.get(label) ?? 0) + 1);
    }
    // Bucket ORDER, not count order: a range chart reads as a scale, so the slices follow the
    // ranges rather than sorting the biggest first.
    // `min`/`max` ride along so a clicked slice can filter by bounds. `max` is omitted for the
    // open-ended top bucket, where Infinity has no JSON form.
    return buckets.map((b, i) => ({
      name: b.name,
      count: counts.get(b.name) ?? 0,
      min: i === 0 ? 1 : buckets[i - 1].max + 1,
      ...(Number.isFinite(b.max) ? { max: b.max } : {}),
    })).filter(r => r.count > 0);
  };

  // LED strips cluster at powers of two, so the ranges follow that rather than decades: a 60-light
  // strip and a 300-light strip are different installations in a way "10-100" and "100-1000" blur.
  const kLightBuckets = [
    { name: "1-64", max: 64 }, { name: "65-256", max: 256 },
    { name: "257-1024", max: 1024 }, { name: "1025-4096", max: 4096 },
    { name: "4097+", max: Infinity },
  ];
  // Decades would put every ESP32 in one bucket. These separate a struggling classic from an S3.
  const kFreeBuckets = [
    { name: "<32 KB", max: 32 * 1024 }, { name: "32-64 KB", max: 64 * 1024 },
    { name: "64-128 KB", max: 128 * 1024 }, { name: "128-256 KB", max: 256 * 1024 },
    { name: "256 KB+", max: Infinity },
  ];
  const kTotalBuckets = [
    { name: "<128 KB", max: 128 * 1024 }, { name: "128-256 KB", max: 256 * 1024 },
    { name: "256-512 KB", max: 512 * 1024 }, { name: "512 KB-4 MB", max: 4 * 1024 * 1024 },
    { name: "4 MB+", max: Infinity },
  ];

  const moduleRoles = await moduleCountsByRole();

  const { results: totals } = await env.DB.prepare(
    `SELECT COUNT(DISTINCT installationId) AS installations, COUNT(*) AS reports
       FROM reports WHERE 1=1${filter}`
  ).bind(...binds).all();

  return json({
    installations: totals?.[0]?.installations ?? 0,
    reports: totals?.[0]?.reports ?? 0,
    // Says which population these figures describe, so a dashboard never has to guess.
    includesDevelopmentInstalls: !releasedOnly,
    // What the reader narrowed to, so the card can show it and offer a way back.
    filtered,
    versions: await counts("version"),
    chips: await counts("chip"),
    deviceModels: await counts("deviceModel"),
    countries: await counts("country"),
    // Already in every report and previously unused: aggregating them costs the device nothing.
    flash: await counts("flash"),
    psram: await counts("psram"),
    sdk: await counts("sdk"),
    previousVersions: await counts("previousVersion"),
    lightCounts: await bucketed("lightCount", kLightBuckets),
    freeMemory: await bucketed("freeHeap", kFreeBuckets),
    totalMemory: await bucketed("totalHeap", kTotalBuckets),
    events: await eventCounts(),
    ...(() => {
      // Spread as `drivers`, `services`, `layouts`, `effects`, … so a new role needs no server
      // change: whatever devices report becomes a chart the UI can render.
      const byRole = moduleRoles;
      return {
        drivers: byRole.driver ?? [],
        services: byRole.service ?? [],
        layouts: byRole.layout ?? [],
        effects: byRole.effect ?? [],
        modifiers: byRole.modifier ?? [],
      };
    })(),
    builds: await buildCounts(),
    updated: new Date().toISOString(),
  });
}

// MoonTalk: a public message board between devices.
//
// Everything here is READABLE BY ANYONE, which is the whole point and also the constraint: there is
// no private message, no recipient and no per-user state, so the only thing to get right is not
// storing more than a sender published. `name` arrives only when that device's own consent said so;
// absent, a message shows a short id prefix instead.
//
// There is NO AUTHENTICATION, so a sender id can be fabricated by anyone who wants to. That is
// acceptable for a hobby board where nothing is gated on identity, and it is stated in the privacy
// policy rather than left to be discovered.
const SENDER_CHARS = 8;    // how much of the sender id a message publishes; the row keeps all 32
const MAX_MESSAGE = 280;   // a message, not a document: bounded so one caller cannot fill the table
const MAX_NAME = 32;
const PAGE_SIZE = 50;

async function handleTalkPost(request, env) {
  let msg;
  try {
    msg = await request.json();
  } catch {
    return json({ error: "not JSON" }, 400);
  }
  if (typeof msg !== "object" || msg === null) return json({ error: "not an object" }, 400);

  // The FULL id is stored and validated; only the first SENDER_CHARS are ever published (see
  // handleTalkGet). Truncating here instead looked like storing less, and broke every post: the
  // length check below rejects anything that is not a whole installation id.
  const sender = typeof msg.sender === "string" ? msg.sender.slice(0, 32) : "";
  const text = typeof msg.text === "string" ? msg.text.trim() : "";
  // The name is OPTIONAL by design: a device shares it only when its own consent control says so,
  // so an empty one is the normal case rather than an error.
  const name = typeof msg.name === "string" ? msg.name.trim().slice(0, MAX_NAME) : "";

  if (!sender || sender.length !== 32) return json({ error: "sender required" }, 400);
  if (!text) return json({ error: "empty message" }, 400);
  if (text.length > MAX_MESSAGE) return json({ error: "message too long" }, 400);

  await env.DB.prepare(
    `INSERT INTO messages (sender, name, text, country, sentAt) VALUES (?, ?, ?, ?, ?)`
  )
    .bind(sender, name, text.slice(0, MAX_MESSAGE), request.cf?.country || "??",
          new Date().toISOString())
    .run();

  return json({ ok: true });
}

async function handleTalkGet(request, env) {
  const url = new URL(request.url);
  // `since` lets a device poll for what it has not seen instead of re-reading the board.
  const since = Number(url.searchParams.get("since") || 0) || 0;

  // An incremental read walks FORWARD from `since`, so a caller that missed more than one page gets
  // the oldest unseen first and can page again. Ordering DESC here would hand back the newest 50 and
  // silently drop everything between, which a caller cannot detect.
  const { results } = await env.DB.prepare(
    since
      ? `SELECT id, sender, name, text, country, sentAt FROM messages
          WHERE id > ? ORDER BY id ASC LIMIT ?`
      : `SELECT id, sender, name, text, country, sentAt FROM messages
          WHERE id > ? ORDER BY id DESC LIMIT ?`
  ).bind(since, PAGE_SIZE).all();

  // The full sender id is NOT published: a caller gets the first 8 characters, which is enough to
  // group one device's messages together and not enough to match against a Stats row.
  const messages = (results ?? []).map((m) => ({
    id: m.id,
    from: m.name || m.sender.slice(0, SENDER_CHARS),
    // The id prefix travels alongside a shared name rather than instead of it, so a named sender
    // is still identifiable as one device: two people can pick the same device name, and without
    // this their messages would be indistinguishable.
    senderId: m.sender.slice(0, SENDER_CHARS),
    named: Boolean(m.name),
    text: m.text,
    country: m.country,
    sentAt: m.sentAt,
  }));

  return json({ messages });
}

// A minimal page at the root, so opening the server in a browser shows what it holds rather than a
// 404. DELIBERATELY SMALL: the device card is the real UI (that is why the API exists at all), and
// this is the view for whoever runs or checks the server. Inline HTML with no build step and no
// dependency, kept in one string so the whole server stays four files.
const PAGE = `<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MoonCloud Stats</title>
<style>
  :root { color-scheme: light dark; }
  body { font: 15px/1.6 system-ui, sans-serif; max-width: 64rem; margin: 3rem auto; padding: 0 1rem; }
  h1 { font-size: 1.5rem; margin-bottom: .2rem; }
  .sub { opacity: .65; margin-bottom: 2.5rem; }
  .charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(18rem, 1fr)); gap: 2.5rem; }
  .chart-section { text-align: center; }
  .chart-title { font-size: .8rem; text-transform: uppercase; letter-spacing: .05em; opacity: .55; margin-bottom: .8rem; }
  .legend { display: inline-block; text-align: left; margin-top: .8rem; font-size: .85rem; }
  .legend div { display: flex; align-items: center; gap: .45rem; line-height: 1.7; }
  .swatch { width: .7rem; height: .7rem; border-radius: 2px; flex: 0 0 auto; }
  .legend .n { margin-left: auto; opacity: .6; font-variant-numeric: tabular-nums; padding-left: 1rem; }
  svg .slice { stroke: #fff; stroke-width: 1.5; }
  @media (prefers-color-scheme: dark) { svg .slice { stroke: #111; } }
  footer { margin-top: 3.5rem; font-size: .85rem; opacity: .6; }
  a { color: inherit; }
</style>
<h1>MoonCloud Stats</h1>
<div class="sub" id="sub">Loading...</div>
<div class="charts" id="out"></div>
<footer>
  Opt-in, one report per install or upgrade. No addresses are stored and the country is resolved at
  the edge. <a href="https://moonmodules.org/projectMM/privacy-policy.html">Privacy policy</a> &middot;
  <a href="https://github.com/MoonModules/projectMM/tree/main/mooncloud">Source</a> &middot;
  <a href="/api/stats">Raw JSON</a>
</footer>
<script>
// Pie charts drawn as plain SVG arcs rather than with a charting library: ten pies is not worth
// 280 KB of D3 from a CDN, and this page has no build step to tree-shake one.
const COLORS = ["#4a9eff", "#ff8c42", "#3ecf8e", "#e8618c", "#a78bfa",
                "#f6c445", "#4dd0e1", "#9aa5b1"];

// An arc from one angle to another. The whole-circle case is separate because an SVG arc whose
// start and end coincide draws NOTHING, so a single-slice pie would render blank.
function arc(cx, cy, r, a0, a1) {
  if (a1 - a0 >= Math.PI * 2 - 1e-9) {
    return "M " + cx + " " + (cy - r) +
           " A " + r + " " + r + " 0 1 1 " + (cx - 0.001) + " " + (cy - r) + " Z";
  }
  const x0 = cx + r * Math.sin(a0), y0 = cy - r * Math.cos(a0);
  const x1 = cx + r * Math.sin(a1), y1 = cy - r * Math.cos(a1);
  return "M " + cx + " " + cy + " L " + x0 + " " + y0 +
         " A " + r + " " + r + " 0 " + (a1 - a0 > Math.PI ? 1 : 0) + " 1 " + x1 + " " + y1 + " Z";
}

function pie(rows) {
  const NS = "http://www.w3.org/2000/svg";
  const total = rows.reduce((s, r) => s + r.count, 0) || 1;
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", "0 0 200 200");
  svg.setAttribute("width", "190");
  svg.setAttribute("height", "190");
  let a = 0;
  rows.forEach((r, i) => {
    const next = a + (r.count / total) * Math.PI * 2;
    const path = document.createElementNS(NS, "path");
    path.setAttribute("d", arc(100, 100, 92, a, next));
    path.setAttribute("fill", COLORS[i % COLORS.length]);
    path.setAttribute("class", "slice");
    const title = document.createElementNS(NS, "title");
    title.textContent = r.name + ": " + r.count +
                        " (" + Math.round((r.count / total) * 100) + "%)";
    path.appendChild(title);
    svg.appendChild(path);
    a = next;
  });
  return svg;
}

function legend(rows) {
  const box = document.createElement("div");
  box.className = "legend";
  rows.forEach((r, i) => {
    const line = document.createElement("div");
    const sw = document.createElement("span");
    sw.className = "swatch";
    sw.style.background = COLORS[i % COLORS.length];
    const name = document.createElement("span");
    name.textContent = r.name || "unknown";
    const n = document.createElement("span");
    n.className = "n";
    n.textContent = r.count;
    line.append(sw, name, n);
    box.appendChild(line);
  });
  return box;
}

// Everything past the 7th slice becomes one "other" wedge: a pie with twenty slivers is unreadable,
// and the long tail is what the raw JSON is for.
// NOT named "top": that is a read-only property of window, so a global function of that name
// throws "Identifier 'top' has already been declared" in a browser and the whole script never
// runs. Node has no window, so node --check passes and only a browser catches it.
function topSlices(rows, keep = 7) {
  if (!rows || rows.length <= keep) return rows || [];
  const head = rows.slice(0, keep);
  const tail = rows.slice(keep).reduce((s, r) => s + r.count, 0);
  return tail ? head.concat([{ name: "other", count: tail }]) : head;
}

fetch("/api/stats").then(r => r.json()).then(d => {
  document.getElementById("sub").textContent =
    d.installations + " installations, " + d.reports + " reports";
  const out = document.getElementById("out");
  for (const [label, rows] of [["Version", d.versions], ["Chip", d.chips],
                               ["Board", d.deviceModels], ["Flash", d.flash],
                               ["PSRAM", d.psram], ["SDK", d.sdk],
                               ["Install or upgrade", d.events],
                               ["Upgraded from", d.previousVersions],
                               ["Drivers", d.drivers],
                               ["Services", d.services],
                               ["Layouts", d.layouts],
                               ["Effects", d.effects],
                               ["Modifiers", d.modifiers],
                               ["Lights", d.lightCounts],
                               ["Free memory", d.freeMemory],
                               ["Total memory", d.totalMemory],
                               ["Build", d.builds], ["Country", d.countries]]) {
    const shown = topSlices(rows);
    if (!shown.length) continue;
    const section = document.createElement("div");
    section.className = "chart-section";
    const title = document.createElement("div");
    title.className = "chart-title";
    title.textContent = label;
    section.append(title, pie(shown), legend(shown));
    out.appendChild(section);
  }
}).catch(() => {
  // An unreachable server is not an empty one: saying "No data yet." on a failed fetch states as
  // fact something this page has no evidence for.
  document.getElementById("sub").textContent = "Cannot reach the server.";
});
</script>`;

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json",
      // The device fetches this from its own web UI, which is served from a different origin
      // (the device itself), so the browser needs to be told this is allowed. Read-only and
      // aggregate-only, so there is nothing here to protect with a narrower origin.
      "access-control-allow-origin": "*",
      "cache-control": "public, max-age=300",
    },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "POST" && url.pathname === "/api/report") {
      return handleReport(request, env);
    }
    if (request.method === "GET" && url.pathname === "/api/stats") {
      return handleStats(env, url);
    }
    if (request.method === "POST" && url.pathname === "/api/talk") {
      return handleTalkPost(request, env);
    }
    if (request.method === "GET" && url.pathname === "/api/talk") {
      return handleTalkGet(request, env);
    }
    if (request.method === "GET" && (url.pathname === "/" || url.pathname === "/index.html")) {
      return new Response(PAGE, {
        headers: { "content-type": "text/html; charset=utf-8", "cache-control": "public, max-age=300" },
      });
    }
    if (request.method === "OPTIONS") {
      return new Response(null, {
        headers: {
          "access-control-allow-origin": "*",
          "access-control-allow-methods": "GET, POST, OPTIONS",
          "access-control-allow-headers": "content-type",
        },
      });
    }
    return json({ error: "not found" }, 404);
  },
};
