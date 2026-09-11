# MoonCloud

The server behind the MoonCloud card, and the only one projectMM talks to. **Stats** takes a report and hands back the aggregates; **Talk** is a public message board between devices. One Worker, one database, one deploy. Published here for the same reason the firmware is, so that "the deployed code is the published code" is checkable rather than taken on trust.

What it stores, and what it deliberately does not, is in [privacy-policy.md](../docs/privacy-policy.md). The design and the reasoning behind the installation id are in [the MoonCloud plan](../docs/history/plans/Plan-20260910%20-%20MoonCloud.md).

## The whole thing

| File | |
|---|---|
| `worker.js` | both endpoints |
| `schema.sql` | the three tables |
| `wrangler.toml` | deployment config |
| `seed.sql` | sample rows for a local run, never deployed |

## Endpoints

**`POST /api/report`** takes one JSON report. Fields outside the allowlist are dropped rather than stored. A report with no installation id is accepted and discarded, since without one it cannot be counted as an installation. Returns `{"ok": true}`.

**`GET /api/stats`** returns the aggregates the device card renders: installation and report totals, plus counts by version, chip, device model and country. Readable by anyone, so it carries only aggregates and never a row.

**`POST /api/talk`** posts one message to the public board: a sender (the installation id), the text, and a device name ONLY when that device's own consent said to share it. Bounded at 280 characters.

**`GET /api/talk`** returns the newest 50 messages, `?since=<id>` for what a caller has not seen. The full sender id is never published: a message carries the device name if one was shared, else the first 8 characters of the id, which groups one device's messages without naming anyone.

## Running it locally

```sh
uv run moondeck/run/run_mooncloud.py --seed     # --seed only the first time
```

`wrangler dev` runs `worker.js` in **workerd, the same runtime Cloudflare uses**, against a local D1 (SQLite under `.wrangler/`). So this is not a stand-in that approximates the server: it is the server, with a local database and a localhost address, and what you verify here is what deploys.

Point a device at it by changing `kMoonCloudUrl` in `src/ui/app.js` and `kHost` in `src/core/MoonCloudModule.h`, which is a rebuild: the address is compiled in rather than configurable, so that one MoonCloud cannot be mistyped into another.

## Pointing a device somewhere else

`server` and `serverPort` are controls on the **MoonCloud** card, so any device can be aimed at any MoonCloud without a rebuild. The port picks the transport: **443 or empty means HTTPS**, anything else means plain HTTP.

Three cases this exists for, and only one of them is development:

**Self-hosting.** The server is published here precisely so someone can run their own. Set `server` to their host and `serverPort` to 443, and that device reports to them instead. Nothing about MoonCloud assumes the address is ours.

**A local server while changing `worker.js`.** Set `server` to the machine's LAN address and `serverPort` to `8787`:

```sh
uv run moondeck/run/run_mooncloud.py --seed
```

That runs the Worker under workerd, the same runtime Cloudflare uses, against a local D1 under `.wrangler/`. **A DEVICE must be given the machine's LAN address, not `127.0.0.1`**, which on a board means the board itself. `httpRequest` also does no name resolution on the plain-HTTP path, so it has to be a dotted-quad IP rather than a hostname.

The local server binds `0.0.0.0` for exactly this reason: wrangler's default is localhost, which answers from the machine and refuses every board on the network.

**Moving to a new address.** The shipped default is a compile-time value in `MoonCloudModule.h`, and the controls are how a device already in the field follows a move without a firmware update.

## Deploying

```sh
npx wrangler d1 create mooncloud-stats          # then put the id in wrangler.toml
npx wrangler d1 execute mooncloud-stats --file=schema.sql --remote
npx wrangler deploy
```

## Changing the schema

Nothing here is fixed. All four were tested against a real D1, and `wrangler d1 execute` is the tool for each: add `--remote` for the deployed database, leave it off for the local one.

```sh
npx wrangler d1 execute mooncloud-stats --remote --command="ALTER TABLE reports ADD COLUMN cpu TEXT NOT NULL DEFAULT ''"
npx wrangler d1 execute mooncloud-stats --remote --command="ALTER TABLE reports DROP COLUMN psram"
npx wrangler d1 execute mooncloud-stats --remote --command="ALTER TABLE reports RENAME COLUMN sdk TO sdkVersion"
npx wrangler d1 execute mooncloud-stats --remote --command="UPDATE reports SET chip='ESP32-S3' WHERE chip='esp32s3'"
```

Deleting rows has its own script, because it is the one operation that cannot be undone by waiting: `moondeck/run/purge_mooncloud.py`, which asks for the row count before touching the deployed database.

**Adding a field needs three edits, and the order matters.** The column first, then `ALLOWED` in `worker.js` so the field is no longer dropped on arrival, then the report builder in `MoonStatsModule.h`. Reversed, devices send a field the server discards.

**`schema.sql` uses `CREATE TABLE IF NOT EXISTS`**, so re-running it on an existing database changes nothing. A new column has to be said out loud with `ALTER TABLE`, and the file updated to match so a fresh deployment gets it too.

**An old device keeps sending the old shape.** A dropped column means its value is discarded, a renamed one means it arrives under a name nothing reads. Neither breaks the device: an unknown field is ignored, and a missing one is simply absent from the row. That is why the allowlist drops rather than rejects.

**What cannot be changed retroactively is a field that was never collected.** A column added in six months is empty for every row before it, and nothing can fill it in. That is the reasoning behind the `events` table and the `dev` flag: both exist now because history cannot be reconstructed later.

## Moving to another host

Nothing here is Cloudflare-specific except the deployment itself. **The data is plain SQLite** and comes out in one command:

```sh
npx wrangler d1 export mooncloud-stats --remote --output=mooncloud-backup.sql
```

That file is `CREATE TABLE` plus `INSERT` statements, which any SQLite, Postgres or MySQL will take with minor dialect edits. Worth running before any schema change, and worth running periodically regardless.

**What would have to be rewritten** is `worker.js`, and it is about 300 lines of routing, an allowlist and four aggregate queries. The Workers-specific parts are three:

- `env.DB.prepare(...).bind(...).all()`, which is D1's client. Any SQL library replaces it.
- `request.cf.country`, the edge-derived country. **This is the one thing genuinely lost**: elsewhere the country comes from a GeoIP lookup on the IP address, which means handling the address, which the privacy policy promises not to store. A different host means either dropping the country or rewriting that promise.
- The `fetch(request, env)` entry point, which is a shape every serverless runtime has an equivalent of.

**The device side needs no change at all.** `server` and `serverPort` are controls, so a device already in the field follows a move without a firmware update, and the default in `MoonCloudModule.h` is a one-line change for new ones.

**What would break if the address moves without warning**: a report is sent once and never retried, so any device that reports while the old address is dead loses that report. Keep both answering until the fleet has moved.

## The two things that are not obvious

**The country never comes from an address we store.** Cloudflare resolves it at the edge and hands it over as `request.cf.country`, so no code here ever sees an IP. That is what makes the privacy policy's promise structural rather than a discipline someone has to maintain in a log config.

**The primary key is (installationId, version).** A device re-reporting the same upgrade overwrites its row, so a row count is a count of installations rather than of retries. It is also why the totals answer "how many installations are on 4.0.0" rather than only "how many upgrade events happened".
