# What MoonCloud changed, and how to undo it

Written 2026-09-10, while MoonCloud was built. Everything here is either a file in this repository or a piece of state that lives OUTSIDE it, and the second kind is the reason this page exists: `git revert` removes the code and leaves the account, the database and the nameservers exactly as they are.

## Outside the repository

**A Cloudflare account**, owned by moonmodules@icloud.com, on the Free plan with no payment method. Free Workers stop at their limits rather than billing, so the account cannot run up a cost while it stays on Free. To undo: delete the Worker and the database in the dashboard, or the whole account.

**A D1 database** named `mooncloud-stats`, id `7966e8d7-1fdd-4231-b28d-9dfc08ea94b1`, region WEUR (Western Europe). Holds three tables and, at the time of writing, one real report from a desktop. To undo: `npx wrangler d1 delete mooncloud-stats`.

**A deployed Worker** at `https://mooncloud-stats.moonmodules.workers.dev`. To undo: `npx wrangler delete mooncloud-stats`.

**A `workers.dev` subdomain**, `moonmodules`, claimed on first deploy. It belongs to the account and cannot be released, only left unused.

**The nameservers for moonmodules.org**, changed at Esmero from `ns3`/`ns4.esmero.nl` to `imani`/`pablo.ns.cloudflare.com`. **This is the only change that can take the docs site offline**, and the only one that touches something the project already had. To undo: put the old nameservers back at Esmero. Esmero keeps its zone file, so the revert is a form submission and a wait, not a rebuild.

The zone as it was before, which is what must survive:

```
moonmodules.org      A      185.199.108.153
moonmodules.org      A      185.199.109.153
moonmodules.org      A      185.199.110.153
moonmodules.org      A      185.199.111.153
moonmodules.org      AAAA   2606:50c0:8000::153
moonmodules.org      AAAA   2606:50c0:8001::153
moonmodules.org      AAAA   2606:50c0:8002::153
moonmodules.org      AAAA   2606:50c0:8003::153
www.moonmodules.org  CNAME  moonmodules.github.io
_discord             TXT    "dh=e25f5f5ee85784a5717c19e33db7a9ccd6eea13b"
```

All ten point at GitHub Pages or verify Discord, and all are set to DNS only rather than proxied: GitHub Pages serves its own certificate, and proxying is a known source of redirect loops with it.

## In the repository

**New: the server** (`mooncloud/`). `worker.js` carries four endpoints and a dev page, `schema.sql` three tables, `wrangler.toml` the deploy config including the database id above, `seed.sql` sample rows for local runs, plus `README.md` and `DEPLOY.md`.

**New: three modules** (`src/core/MoonCloudModule.h`, `MoonStatsModule.h`, `MoonTalkModule.h`). MoonCloud is the container and owns the server address, the send, and the installation id; Stats and Talk are children with their own consent.

**New: SHA-256** (`src/core/sha256.{h,cpp}`), vendored rather than linked, because ESP-IDF ships mbedtls and the desktop would link something else, and one function with two implementations is one function that drifts.

**New: tooling.** `moondeck/run/run_mooncloud.py` runs the server locally under the same runtime Cloudflare uses; `moondeck/run/purge_mooncloud.py` deletes rows by date range or by development build, authenticated by the Cloudflare login rather than by a public endpoint.

**Changed: `platform::httpsPost`** (`platform.h`, `platform_desktop.cpp`, `platform_esp32_ota.cpp`). One outbound HTTPS call using the TLS each OS already ships: libcurl on desktop, `esp_http_client` with the OTA path's certificate bundle on ESP32. libcurl is OPTIONAL: without it the build succeeds and reporting is disabled, because a build must never fail over an opt-in statistic.

**Changed: the desktop reports its real architecture.** `chipModel()` returned the literal "desktop"; it now returns `arm64` / `x64`, and `hostPlatform()` reports `macos-arm64` / `linux-x64` / `windows-x64` / `docker` using the vocabulary the release packaging already uses.

**Fixed: `sha256.cpp` was missing from the ESP32 build** (`esp32/main/CMakeLists.txt`), so MoonCloud had never compiled for a device. Found by building for the Olimex, which failed to link.

**Changed: the privacy policy**, rewritten from 142 lines to 79 and made future-proof: it states a RULE (nothing is sent unless you switch it on) rather than an inventory of one feature, and names usage data and crash reports as things that may later be offered as their own opt-ins.

## The address is `stats.moonmodules.org`

`MoonCloudModule.h` (`kHost`) and `app.js` (`kMoonCloudUrl`) both compile in **`stats.moonmodules.org`** on port 443, switched together once the zone moved to Cloudflare and the Worker got its Custom Domain.

The `workers.dev` address still answers the same Worker, so nothing that shipped pointing at it is stranded. Keeping it alive takes an explicit `workers_dev = true` in `wrangler.toml`: declaring any route disables it by default, and the first deploy of the Custom Domain did exactly that, returning 404 on the address the firmware had compiled in until the line was added.

To undo: set both constants back to `mooncloud-stats.moonmodules.workers.dev` and rebuild. Both addresses serve the same Worker and the same D1 database, so the switch is reversible without touching data.

## Before merging to main

Two things, neither of which a test can catch.

**1. Point at the real domain. DONE.** `kHost` and `kMoonCloudUrl` both read `stats.moonmodules.org`, changed together: a device reporting to one address while the card reads another shows a user their own report missing. Verified answering over HTTPS with a valid certificate.

**2. Empty both databases.** Everything in them is test data from the evening MoonCloud was built: probe reports, a board reflashed repeatedly, messages typed to watch the board update. Shipping with it means the first real user sees a version distribution describing one developer's bench.

```sh
uv run moondeck/run/purge_mooncloud.py --from 2026-01-01 --to 2026-12-31 --remote
```

That asks for the row count before touching the deployed database. The local one under `.wrangler/` is scratch and can be left alone, or cleared the same way without `--remote`.

## Measured cost on a device

+12,832 bytes of flash (+0.77%) and about 800 bytes of static RAM, measured on a classic ESP32 by building the same tree with and without MoonCloud on one toolchain. The TLS stack was already linked for OTA, so `httpsPost` adds call-site code rather than a library.
