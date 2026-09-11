#!/usr/bin/env python3
"""Run the MoonCloud Stats server locally, on the same code that ships to Cloudflare.

`wrangler dev` executes `mooncloud/worker.js` in workerd, the SAME runtime Cloudflare runs in
production, against a local D1 (SQLite on disk under .wrangler/). So this is not a stand-in that
approximates the server: it is the server, with a local database and a localhost address. What you
verify here is what deploys.

The one thing that differs is `request.cf.country`, which the edge fills in and a local run leaves
undefined. The worker already handles that (an unknown country is stored as "??"), so the local
path exercises the same branch a report from an unrecognized network would take.

Deploying afterwards is `npx wrangler deploy` from mooncloud/, and nothing about the code changes.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MOONCLOUD = ROOT / "mooncloud"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8787,
                    help="port to serve on (default 8787, wrangler's own default)")
    ap.add_argument("--seed", action="store_true",
                    help="apply schema.sql and insert a few sample reports first, so "
                         "GET /api/stats has something to return")
    args = ap.parse_args()

    if shutil.which("npx") is None:
        print("npx not found: install Node.js (https://nodejs.org) to run the local server.",
              file=sys.stderr)
        return 1

    if args.seed:
        print("Applying schema and sample rows to the local D1...")
        # --local keeps this on the on-disk SQLite under .wrangler/, never the deployed database.
        for sql in ("schema.sql", "seed.sql"):
            r = subprocess.run(
                ["npx", "wrangler", "d1", "execute", "mooncloud-stats", "--local", f"--file={sql}"],
                cwd=MOONCLOUD)
            if r.returncode != 0:
                print(f"failed applying {sql}", file=sys.stderr)
                return r.returncode

    print(f"MoonCloud Stats on http://localhost:{args.port} (and on this machine's LAN address, for devices)")
    print(f"  POST http://localhost:{args.port}/api/report")
    print(f"  GET  http://localhost:{args.port}/api/stats")
    print("Ctrl-C to stop.")
    # 0.0.0.0, not wrangler's default localhost: a DEVICE reporting to this server is the whole
    # point of running it here, and a localhost-only bind refuses every board on the LAN while
    # answering fine from this machine, which looks like a firmware bug rather than a bind address.
    return subprocess.run(
        ["npx", "wrangler", "dev", "--local", "--ip", "0.0.0.0", "--port", str(args.port)],
        cwd=MOONCLOUD).returncode


if __name__ == "__main__":
    sys.exit(main())
