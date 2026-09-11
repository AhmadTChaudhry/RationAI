#!/usr/bin/env python3
"""Probe Claude Code's own usage endpoint.

Claude Code doesn't read quota from the rate-limit headers alone -- it GETs
`/api/oauth/usage`. Decompiled from the CLI binary:

    let r = atWall ? "/api/oauth/usage?at_wall=1&skip_spend=1" : "/api/oauth/usage"
    let p = await http.get(r, { timeout: 5000,
                                headers: {"Content-Type": "application/json"},
                                refreshOAuth: true, credentials })

If that works for us too it is strictly better than the Haiku probe in
claude_live.py: a plain GET, no inference billed, and the response carries
per-model weekly limits ("weekly_scoped" entries with scope.model.display_name)
that the headers never expose.

The catch is the token. Claude Code passes refreshOAuth:true and renews on a
401; we deliberately don't, so this only works while the access token is live.
Run with --wait to sit until Claude Code next renews it, then fire immediately.

    python3 probe_usage_endpoint.py            # try now
    python3 probe_usage_endpoint.py --wait     # wait for a valid token, then try
"""
import argparse
import json
import time
import urllib.error
import urllib.request

import claude_live

# Both hosts appear in the CLI binary; the endpoint is relative there, so try
# each rather than guess which base URL it resolves against.
HOSTS = ["https://api.anthropic.com", "https://claude.ai"]
PATHS = ["/api/oauth/usage?at_wall=1&skip_spend=1", "/api/oauth/usage"]


def attempt(host, path, token):
    request = urllib.request.Request(
        host + path,
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "anthropic-beta": "oauth-2025-04-20",
            "anthropic-version": "2023-06-01",
            "User-Agent": "claude-code/2.1.5",
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except (urllib.error.URLError, OSError) as e:
        return None, str(e).encode()


def token_now():
    """(token, seconds_until_expiry) -- expiry is None when not recorded."""
    creds = claude_live.read_credentials() or {}
    token = creds.get("accessToken")
    expires_at = claude_live.token_expiry(creds)
    left = (expires_at - time.time()) if expires_at else None
    return token, left


def wait_for_token(timeout_s):
    """Poll the credential store until Claude Code renews the access token."""
    deadline = time.time() + timeout_s
    warned = False
    while time.time() < deadline:
        token, left = token_now()
        if token and (left is None or left > 30):
            print(f"token is live ({left/60:.1f} min remaining)" if left else "token found")
            return token
        if not warned:
            print("waiting for Claude Code to renew the token -- run any CLI command "
                  "to trigger it (Ctrl-C to stop)")
            warned = True
        time.sleep(10)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wait", action="store_true",
                    help="poll until the token is valid, then probe")
    ap.add_argument("--timeout", type=int, default=3600, help="seconds to wait")
    ap.add_argument("--out", default="usage_endpoint_response.json")
    args = ap.parse_args()

    token, left = token_now()
    if not token:
        print("no Claude Code credentials found")
        return 1
    if left is not None and left <= 30:
        print(f"access token expired {-left/60:.1f} min ago")
        if not args.wait:
            print("re-run with --wait, or run any Claude Code command first")
            return 1
        token = wait_for_token(args.timeout)
        if not token:
            print("gave up waiting")
            return 1

    for host in HOSTS:
        for path in PATHS:
            status, body = attempt(host, path, token)
            label = f"{host}{path}"
            if status is None:
                print(f"  {label}\n    network error: {body.decode()[:120]}")
                continue
            print(f"  {label}\n    HTTP {status}  ({len(body)} bytes)")
            if status != 200:
                print(f"    {body[:200].decode('utf-8', 'replace')}")
                continue

            try:
                data = json.loads(body)
            except json.JSONDecodeError:
                print(f"    non-JSON: {body[:200].decode('utf-8', 'replace')}")
                continue

            with open(args.out, "w") as fh:
                json.dump(data, fh, indent=2)
            print(f"    saved -> {args.out}")
            print(json.dumps(data, indent=2)[:3000])
            return 0

    print("\nno variant returned 200 -- the endpoint may need a session cookie "
          "or a header the CLI sets that we haven't replicated")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
