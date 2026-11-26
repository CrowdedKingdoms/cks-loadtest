#!/usr/bin/env python3
"""Counts or deletes an organization's managed players made in a window of time: the players a load test made by
joining with external ids nobody had signed in with.

    managed_players.py count  --secrets-env <file> --since 2026-11-25T23:10:00Z [--until ...]
    managed_players.py delete --secrets-env <file> --since 2026-11-25T23:10:00Z [--until ...] --yes

The secrets file is the mesh's (CROWDY_API_ORIGIN, CROWDY_ORG_TOKEN with players:manage); the token is never
printed. `managedPlayers` lists every player of the token's organization, a page of 200 at a time; this keeps the
ones whose createdAt falls in the window. `delete` asks `deleteManagedPlayer` for each, a few at a time and no
faster than --rate a second, and waits when the API says it is busy (extensions.retryAfterMs). An organization
holds 100,000 players at most, and a deleted player is an erasure record on the tier: delete what a test made, and
nothing else.
"""
import argparse
import concurrent.futures
import datetime
import json
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

LIST = "query($o: BigInt!, $a: String) { managedPlayers(orgId: $o, first: 200, after: $a) { players { externalId createdAt } nextCursor } }"
DELETE = "mutation($o: BigInt!, $e: String!) { deleteManagedPlayer(orgId: $o, externalId: $e) }"


def secrets(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            out[key.strip()] = value.strip().strip("'\"")
    return out


def post(origin, token, document, variables):
    request = urllib.request.Request(origin.rstrip("/") + "/graphql", data=json.dumps({"query": document, "variables": variables}).encode(),
                                     headers={"content-type": "application/json", "authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(request, timeout=60) as reply:
            return json.loads(reply.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read() or b"{}")
        except ValueError:
            return {"errors": [{"message": f"HTTP {e.code}"}]}
    except OSError as e:
        return {"errors": [{"message": str(e)}]}


def asked(origin, token, document, variables, tries=8):
    """The reply's data, waiting as long as the API says when it is busy."""
    for _ in range(tries):
        reply = post(origin, token, document, variables)
        errors = reply.get("errors")
        if not errors:
            return reply.get("data") or {}
        extensions = errors[0].get("extensions") or {}
        if extensions.get("code") in ("PLATFORM_BUSY", "RATE_LIMITED") or extensions.get("retryAfterMs"):
            time.sleep(min(10.0, max(0.25, (extensions.get("retryAfterMs") or 1500) / 1000)))
            continue
        raise RuntimeError(f"{extensions.get('code', 'ERROR')}: {errors[0].get('message', '')[:200]}")
    raise RuntimeError("the API stayed busy")


def when(text):
    return datetime.datetime.fromisoformat(text.replace("Z", "+00:00"))


def made_in(origin, token, org, since, until):
    out, total, cursor = [], 0, None
    while True:
        page = asked(origin, token, LIST, {"o": org, "a": cursor})["managedPlayers"]
        for player in page["players"]:
            total += 1
            made = when(player["createdAt"])
            if since <= made <= until:
                out.append(player["externalId"])
        cursor = page.get("nextCursor")
        if not cursor:
            return out, total


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["count", "delete"])
    p.add_argument("--secrets-env", required=True)
    p.add_argument("--org", required=True, help="the organization's id")
    p.add_argument("--since", required=True, help="players made at or after this (UTC, ISO 8601)")
    p.add_argument("--until", help="and at or before this (default: now)")
    p.add_argument("--rate", type=float, default=10.0, help="deletions a second at most")
    p.add_argument("--yes", action="store_true", help="delete: really")
    args = p.parse_args()
    env = secrets(args.secrets_env)
    origin, token = env["CROWDY_API_ORIGIN"], env["CROWDY_ORG_TOKEN"]
    since = when(args.since)
    until = when(args.until) if args.until else datetime.datetime.now(datetime.timezone.utc)
    ids, total = made_in(origin, token, args.org, since, until)
    print(f"{len(ids)} of the organization's {total} managed players were made from {since.isoformat()} to {until.isoformat()}", flush=True)
    if args.command == "count":
        return 0
    if not args.yes:
        print("delete: pass --yes to delete them")
        return 2
    lock, state = threading.Lock(), {"next": time.time(), "done": 0, "failed": 0}

    def delete(external_id):
        with lock:
            wait = state["next"] - time.time()
            state["next"] = max(state["next"], time.time()) + 1.0 / args.rate
        if wait > 0:
            time.sleep(wait)
        try:
            ok = asked(origin, token, DELETE, {"o": args.org, "e": external_id}).get("deleteManagedPlayer")
        except RuntimeError as e:
            ok = False
            with lock:
                if state["failed"] < 3:
                    print(f"  {e}")
        with lock:
            state["done" if ok else "failed"] += 1
            if (state["done"] + state["failed"]) % 200 == 0:
                print(f"  {state['done']} deleted, {state['failed']} failed of {len(ids)}", flush=True)

    # A run that is stopped still says how far it got: the count is what the next run's --since and --until need.
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=4)
    try:
        list(pool.map(delete, ids))
    except KeyboardInterrupt:
        pool.shutdown(wait=False, cancel_futures=True)
        print(f"stopped: deleted {state['done']}, failed {state['failed']} of {len(ids)}", flush=True)
        return 130
    pool.shutdown()
    print(f"deleted {state['done']}, failed {state['failed']}", flush=True)
    return 0 if state["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
