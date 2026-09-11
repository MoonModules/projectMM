#!/usr/bin/env python3
"""Delete MoonCloud rows in a date range, local or deployed.

Test runs leave rows behind: a bench board reporting under a dev build, a dozen probe messages, a
day of one board reflashed twenty times. This removes them by DAY, which is the resolution the
tables store.

WHY A SCRIPT AND NOT AN ENDPOINT. Every route in `mooncloud/worker.js` is unauthenticated, which is
right for reports and a public board and would be wrong for a delete: a public DELETE with a date
range lets anyone erase the whole dataset with one call. `wrangler d1 execute` is already
authenticated by the Cloudflare login and already works both ways, so the safe version is the one
that needs no new code on the server at all.

    uv run moondeck/run/purge_mooncloud.py --from 2026-09-01 --to 2026-09-10
    uv run moondeck/run/purge_mooncloud.py --from 2026-09-10 --to 2026-09-10 --remote
    uv run moondeck/run/purge_mooncloud.py --dev-only          # just development-build rows

Shows what it would remove and asks before removing it.

`--yes` skips the question for the LOCAL database only. The deployed one always asks, and asks for
the row count rather than a keystroke: those rows came from real devices, each sent once, and a
report is never retried, so a delete there is not recoverable by waiting.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MOONCLOUD = ROOT / "mooncloud"

DAY = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def run_sql(sql: str, remote: bool) -> str:
    """One `wrangler d1 execute`, returning its output."""
    cmd = ["npx", "wrangler", "d1", "execute", "mooncloud-stats",
           "--remote" if remote else "--local", f"--command={sql}"]
    r = subprocess.run(cmd, cwd=MOONCLOUD, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise SystemExit(r.returncode)
    return r.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="start", help="first day to delete, YYYY-MM-DD")
    ap.add_argument("--to", dest="end", help="last day to delete, YYYY-MM-DD (inclusive)")
    ap.add_argument("--dev-only", action="store_true",
                    help="only rows from locally-built firmware, whatever the date")
    ap.add_argument("--remote", action="store_true",
                    help="the DEPLOYED database (default: the local one under .wrangler/)")
    ap.add_argument("--yes", action="store_true",
                    help="skip the question. LOCAL ONLY: the deployed database always asks, "
                         "because the rows there came from other people's devices and cannot "
                         "be sent again")
    args = ap.parse_args()

    if shutil.which("npx") is None:
        print("npx not found: install Node.js to reach the database.", file=sys.stderr)
        return 1

    for day in (args.start, args.end):
        if day and not DAY.match(day):
            print(f"not a date: {day} (want YYYY-MM-DD)", file=sys.stderr)
            return 1
    if not args.dev_only and not (args.start and args.end):
        print("give --from and --to, or --dev-only", file=sys.stderr)
        return 1

    # `reports` dates its rows `receivedAt`, `events` calls the same thing `day`, and `messages`
    # carries a full timestamp whose first ten characters are the date.
    if args.dev_only:
        where = {"reports": "dev = 1", "events": "dev = 1", "messages": None}
        what = "development-build rows"
    else:
        where = {
            "reports": f"receivedAt BETWEEN '{args.start}' AND '{args.end}'",
            "events": f"day BETWEEN '{args.start}' AND '{args.end}'",
            "messages": f"substr(sentAt, 1, 10) BETWEEN '{args.start}' AND '{args.end}'",
        }
        what = f"rows from {args.start} to {args.end} inclusive"

    where = {t: w for t, w in where.items() if w}

    print(f"About to delete {what} from the "
          f"{'DEPLOYED' if args.remote else 'local'} database:\n")
    total = 0
    for table, cond in where.items():
        out = run_sql(f"SELECT COUNT(*) AS n FROM {table} WHERE {cond}", args.remote)
        m = re.search(r'"n":\s*(\d+)', out)
        n = int(m.group(1)) if m else 0
        total += n
        print(f"  {table:10} {n:>6} rows")

    if total == 0:
        print("\nNothing to delete.")
        return 0

    # A remote delete ALWAYS asks, and asks for the number rather than a keystroke. The local
    # database is scratch that a re-run rebuilds; the deployed one holds reports that real devices
    # sent once and will never send again, because a report is not retried. `--yes` is for a script
    # against local data, and letting it through here would make the destructive case the easy one.
    if args.remote:
        print()
        print("This is the DEPLOYED database. These rows came from real devices and cannot be")
        print("sent again: a report is one-time and is never retried.")
        # EOFError, not a crash: run without a terminal (a script, a pipe) there is nobody to ask,
        # and the safe reading of "nobody answered" is to leave the rows alone.
        try:
            answer = input(f"Type {total} to delete, anything else to stop: ").strip()
        except EOFError:
            print("\nNot a terminal, so nothing was asked and nothing was deleted.")
            return 1
        if answer != str(total):
            print("Left alone.")
            return 0
    elif not args.yes:
        print()
        try:
            answer = input(f"Delete {total} rows? [y/N] ").strip().lower()
        except EOFError:
            print("\nNot a terminal, so nothing was asked and nothing was deleted.")
            return 1
        if answer not in ("y", "yes"):
            print("Left alone.")
            return 0

    for table, cond in where.items():
        run_sql(f"DELETE FROM {table} WHERE {cond}", args.remote)
    print(f"\nDeleted {total} rows.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
