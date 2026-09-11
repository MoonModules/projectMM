# Deploying MoonCloud

Three of these steps only the account owner can do, and they are the reason this is not automated: the account is a standing commitment rather than a build artifact.

## Before anything

**Decide who owns the Cloudflare account.** It needs a person, not a project: someone whose login can deploy, whose email gets the alerts, and who notices when it breaks. If that person leaves, the server does too. This is the question that kept the feature in the backlog, and it is not a technical one.

Stats and Talk run on **Workers plus D1, both free tier**. Sync would need Durable Objects, which are paid.

## 1. Log in

```sh
npx wrangler login
```

Opens a browser against the owner's account. Nothing below works until this succeeds.

## 2. Create the database

```sh
cd mooncloud
npx wrangler d1 create mooncloud-stats
```

It prints a `database_id`. Put it in `wrangler.toml`, replacing `REPLACE_WITH_D1_DATABASE_ID`, and commit that: the id is not a secret, and a config that cannot deploy without a local edit is a config that drifts.

## 3. Create the tables

```sh
npx wrangler d1 execute mooncloud-stats --file=schema.sql --remote
```

`--remote` is the deployed database; without it you get the local one under `.wrangler/`.

**On a database that already exists**, `CREATE TABLE IF NOT EXISTS` skips it, so a new column needs saying out loud:

```sh
npx wrangler d1 execute mooncloud-stats --remote \
  --command="ALTER TABLE reports ADD COLUMN dev INTEGER NOT NULL DEFAULT 0"
```

## 4. Deploy

```sh
npx wrangler deploy
```

Answers on `mooncloud-stats.<subdomain>.workers.dev` immediately. Check it:

```sh
curl https://mooncloud-stats.<subdomain>.workers.dev/api/stats
```

## 5. Point a domain at it

`stats.moonmodules.org` needs the zone on Cloudflare (its nameservers pointed there), then a Custom Domain route in `wrangler.toml`:

```toml
[[routes]]
pattern = "stats.moonmodules.org"
custom_domain = true
```

Then deploy again. Cloudflare creates the DNS record and issues the certificate itself, so there is no record to add by hand.

**Declaring any route disables the `workers.dev` address by default**, and that address is the one older firmware compiled in. Keep it alive with an explicit line, or every device still pointing at it gets a 404:

```toml
workers_dev = true
```

**Until this is done the firmware default below cannot be set**, because a `workers.dev` name is not a promise: it moves with the account.

## 6. Point the firmware at it

The address is compiled in, not a setting: `kHost` in `MoonCloudModule.h` and `kMoonCloudUrl` in `src/ui/app.js`. They must change TOGETHER, because a device reporting to one address while the card reads another shows a user their own report missing.

```cpp
static constexpr const char* kHost = "stats.moonmodules.org";
static constexpr uint16_t kPort = 443;   // 443 selects HTTPS
```

**This is the last step, not the first.** Ship it before the domain answers and every device in the wild sends one report into nothing, and never retries: a failed report is not queued.

## Afterwards

- `uv run moondeck/run/purge_mooncloud.py --from ... --to ... --remote` removes rows by date, authenticated by this login rather than by a public endpoint.
- The privacy policy names the server: check it still describes what runs.
