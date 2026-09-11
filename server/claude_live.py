"""Real Claude quota, straight from the API's rate-limit response headers.

Approach borrowed from Clawdmeter (github.com/HermannBjorgvin/Clawdmeter):
Claude Code already holds an OAuth access token on this machine, so we reuse it
to make one throwaway 1-token Haiku call and read the quota out of the response
headers.

We deliberately never refresh: refresh tokens generally rotate, so renewing
behind Claude Code's back could invalidate the CLI's own credentials and log
you out. Claude Code owns refreshing. The cost of that choice is that an idle
CLI eventually means a dead token, so a stale-but-real reading is served in
the meantime rather than a blank panel.
"""
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_FILE = Path.home() / ".claude" / ".credentials.json"

# Claude Code's own source of truth for the /usage panel: a plain GET, no
# inference billed, and richer than the response headers (exact money with a
# currency, server-computed severity, and per-model weekly buckets).
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

# Fallback only: a 1-token Haiku call made purely to read the
# anthropic-ratelimit-unified-* headers off the response.
API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

# The probe is cheap but not free; don't let a chatty display drive the rate.
MIN_POLL_SECONDS = 60

# Only used by the header fallback, which reports credits as a bare fraction
# with no currency. The usage endpoint returns the real amount and currency,
# so this figure is a last resort.
CREDIT_CAP = 70.0
_cache = {"at": 0.0, "value": None}
# Last reading that actually came back from the API. Kept separately from the
# poll cache so an expired token degrades to "stale" instead of "blank".
_last_good = {"at": 0.0, "value": None}


def _extract_token(blob):
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        # Not JSON -- fall back to spotting the token in the raw text.
        match = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
        return match.group(1) if match else None
    if isinstance(data, dict):
        oauth = data.get("claudeAiOauth")
        if isinstance(oauth, dict) and oauth.get("accessToken"):
            return oauth["accessToken"]
        if data.get("accessToken"):
            return data["accessToken"]
    return None


def _decode_keychain_blob(raw):
    """`security -w` sometimes hex-dumps the secret instead of printing text."""
    stripped = raw.strip()
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2})+", stripped) and len(stripped) > 32:
        try:
            return bytes.fromhex(stripped).decode("utf-8", "replace")
        except ValueError:
            pass
    return raw


def read_credentials():
    """The stored OAuth blob as a dict, or None."""
    blob = None
    try:
        if CREDENTIALS_FILE.exists():
            blob = CREDENTIALS_FILE.read_text()
    except OSError:
        pass
    if not blob and sys.platform == "darwin":
        try:
            out = subprocess.run(
                ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
                capture_output=True, text=True, timeout=15,
            )
            if out.returncode == 0:
                blob = _decode_keychain_blob(out.stdout)
        except (OSError, subprocess.SubprocessError):
            return None
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        token = _extract_token(blob)
        return {"accessToken": token} if token else None
    if isinstance(data, dict):
        return data.get("claudeAiOauth") if isinstance(data.get("claudeAiOauth"), dict) else data
    return None


def token_expiry(creds):
    """When the access token dies, in epoch seconds. expiresAt is in ms (JS)."""
    value = (creds or {}).get("expiresAt")
    try:
        return float(value) / 1000.0
    except (TypeError, ValueError):
        return None


def _probe(token):
    request = urllib.request.Request(
        API_URL,
        data=json.dumps(API_BODY).encode(),
        headers={**API_HEADERS, "Authorization": f"Bearer {token}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as resp:
            return resp.headers, resp.status
    except urllib.error.HTTPError as e:
        # 429 still carries the rate-limit headers, and that is the interesting case.
        return e.headers, e.code
    except (urllib.error.URLError, OSError) as e:
        raise ConnectionError(str(e)) from e


def _pct(value):
    try:
        return round(float(value) * 100, 1)
    except (TypeError, ValueError):
        return 0.0


def _epoch(value):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def _clock_label(epoch):
    """'Fri 8:00 PM' for a reset far enough out that a countdown reads badly."""
    if not epoch:
        return None
    local = time.localtime(epoch)
    hour = local.tm_hour % 12 or 12
    ampm = "AM" if local.tm_hour < 12 else "PM"
    return f"{time.strftime('%a', local)} {hour}:{local.tm_min:02d} {ampm}"


def _resets_in(value, now):
    try:
        remaining = float(value) - now
    except (TypeError, ValueError):
        return None
    return int(remaining) if remaining > 0 else 0


def _degraded(reason, now):
    """Prefer a stale-but-real reading over nothing at all -- but only where it
    still describes something real.

    A snapshot only describes the window it was taken in. Once that window's
    reset has passed, replaying the number is not "old data" but a wrong answer
    about a window that no longer exists: a 5-hour reading from 9 hours ago has
    rolled over at least once.
    """
    if not _last_good["value"]:
        return {"available": False, "reason": reason}

    out = dict(_last_good["value"])
    for name in ("session", "week", "credits"):
        window = out.get(name)
        if not isinstance(window, dict):
            continue
        resets_at = window.get("resets_at")
        if resets_at and resets_at < now:
            window = dict(window)
            window["percent"] = 0.0
            window["rolled_over"] = True
            window["resets_in"] = None
            out[name] = window

    out["stale_for"] = int(now - _last_good["at"])
    out["reason"] = reason
    return out


def _iso_to_epoch(value):
    """'2026-09-11T10:00:00.417376+00:00' -> epoch seconds."""
    if not value:
        return None
    try:
        from datetime import datetime
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (ValueError, AttributeError):
        return None


def _money(node):
    """{amount_minor, currency, exponent} -> (float, symbol) or (None, '')."""
    if not isinstance(node, dict):
        return None, ""
    try:
        amount = float(node["amount_minor"]) / (10 ** int(node.get("exponent", 2)))
    except (KeyError, TypeError, ValueError):
        return None, ""
    code = node.get("currency") or ""
    # Enough to disambiguate the common cases; the code is passed through too.
    symbol = {"USD": "$", "AUD": "A$", "GBP": "£", "EUR": "€"}.get(code, code + " ")
    return amount, symbol


def _from_endpoint(payload, now):
    """Normalise /api/oauth/usage into the shape the display expects."""
    windows = {}

    # `limits` is the self-describing surface; the sibling top-level keys
    # include unreleased codenames (tangelo, iguana_necktie, ...) that plainly
    # aren't a contract, so read the array instead.
    scoped = []
    for entry in payload.get("limits") or []:
        if not isinstance(entry, dict):
            continue
        kind = entry.get("kind")
        resets_at = _iso_to_epoch(entry.get("resets_at"))
        window = {
            "percent": round(float(entry.get("percent") or 0), 1),
            "resets_in": max(0, int(resets_at - now)) if resets_at else None,
            "resets_at": int(resets_at) if resets_at else None,
            "resets_label": _clock_label(int(resets_at)) if resets_at else None,
            "severity": entry.get("severity"),
            "is_active": bool(entry.get("is_active")),
        }
        if kind == "session":
            windows["session"] = window
        elif kind == "weekly_all":
            windows["week"] = window
        elif kind == "weekly_scoped":
            model = ((entry.get("scope") or {}).get("model") or {}).get("display_name")
            window["model"] = model
            scoped.append(window)

    # Usage credits, with the real currency rather than an assumed one.
    spend = payload.get("spend")
    if isinstance(spend, dict) and spend.get("enabled"):
        used, symbol = _money(spend.get("used"))
        cap, _ = _money(spend.get("limit"))
        resets_at = _iso_to_epoch(spend.get("resets_at"))
        credits = {
            "percent": round(float(spend.get("percent") or 0), 1),
            "resets_in": max(0, int(resets_at - now)) if resets_at else None,
            "resets_label": _clock_label(int(resets_at)) if resets_at else None,
            "severity": spend.get("severity"),
        }
        if used is not None and cap is not None:
            credits["spent"] = round(used, 2)
            credits["cap"] = round(cap, 2)
            credits["currency"] = (spend.get("used") or {}).get("currency")
            # The device shouldn't have to know about currencies.
            credits["value_label"] = f"{symbol}{used:.0f} of {symbol}{cap:.0f}"
        windows["credits"] = credits

    if not windows:
        return None

    active = next((k for k, w in windows.items() if w.get("is_active")), None)
    result = {
        "available": True,
        "account": "subscription",
        "source": "endpoint",
        "binding": active or max(windows, key=lambda k: windows[k]["percent"]),
        **windows,
    }
    if scoped:
        result["per_model_week"] = scoped
    return result


def usage(force=False):
    now = time.time()
    if not force and _cache["value"] and now - _cache["at"] < MIN_POLL_SECONDS:
        return {**_cache["value"], "cached_for": int(now - _cache["at"])}

    creds = read_credentials()
    token = (creds or {}).get("accessToken")
    if not token:
        return _degraded("no Claude Code OAuth token found", now)

    # Claude Code refreshes this token only when it is itself running, so an
    # idle CLI means a dead token. Check before spending an API call on it.
    expires_at = token_expiry(creds)
    if expires_at and expires_at <= now + 30:
        mins = max(0, int((now - expires_at) / 60))
        return _degraded(f"token expired {mins}m ago -- run any Claude Code command", now)

    # Preferred path: Claude Code's own usage endpoint.
    try:
        request = urllib.request.Request(
            USAGE_URL,
            headers={**API_HEADERS, "Authorization": f"Bearer {token}"},
            method="GET",
        )
        with urllib.request.urlopen(request, timeout=15) as response:
            endpoint = _from_endpoint(json.loads(response.read()), now)
        if endpoint:
            _cache["at"] = now
            _cache["value"] = endpoint
            _last_good["at"] = now
            _last_good["value"] = endpoint
            return {**endpoint, "cached_for": 0}
    except (urllib.error.HTTPError, urllib.error.URLError, OSError,
            json.JSONDecodeError, ValueError, KeyError, TypeError):
        pass  # fall through to the header probe

    try:
        headers, status = _probe(token)
    except ConnectionError as e:
        return _degraded(f"network: {e}", now)

    if status in (401, 403):
        return _degraded("token rejected -- run any Claude Code command", now)

    def hdr(name):
        return headers.get(name)

    # These three windows are independent and can all be present at once --
    # the same three rows Claude Code's own /usage panel shows. Overage is not
    # a fallback for the 5h window; read every one that is offered.
    windows = {}
    for key, prefix in (
        ("session", "5h"),
        ("week", "7d"),
        ("credits", "overage"),
    ):
        util = hdr(f"anthropic-ratelimit-unified-{prefix}-utilization")
        if util is None:
            continue
        resets_at = _epoch(hdr(f"anthropic-ratelimit-unified-{prefix}-reset"))
        window = {
            "percent": _pct(util),
            "resets_in": _resets_in(hdr(f"anthropic-ratelimit-unified-{prefix}-reset"), now),
            "resets_at": resets_at,
            # Wall-clock label in *this machine's* timezone, the way Claude
            # Code writes it ("Fri 8:00 PM"). The device has no clock of its
            # own, so it can only render a string we hand it.
            "resets_label": _clock_label(resets_at),
            "status": hdr(f"anthropic-ratelimit-unified-{prefix}-status") or "unknown",
        }
        if CREDIT_CAP and key == "credits":
            window["spent"] = round(CREDIT_CAP * window["percent"] / 100, 2)
            window["cap"] = CREDIT_CAP
        windows[key] = window

    if not windows:
        return {"available": False, "reason": f"no rate-limit headers (HTTP {status})"}

    # Which limit is actually the binding one right now.
    claim = hdr("anthropic-ratelimit-unified-representative-claim") or ""
    binding = {"five_hour": "session", "seven_day": "week", "overage": "credits"}.get(claim)

    result = {
        "available": True,
        "account": "subscription",
        "source": "headers",
        "binding": binding,
        **windows,
    }

    _cache["at"] = now
    _cache["value"] = result
    _last_good["at"] = now
    _last_good["value"] = result
    return {**result, "cached_for": 0}


if __name__ == "__main__":
    print(json.dumps(usage(force=True), indent=2))
