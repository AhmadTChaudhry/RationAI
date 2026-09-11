"""Live ChatGPT/Codex plan allowance.

The Codex app authenticates with a ChatGPT subscription rather than a platform
API key, and it reads its own allowance from an endpoint found in the app
binary (`/api/codex/usage` against the ChatGPT backend):

    GET https://chatgpt.com/backend-api/codex/usage

This is the real plan allowance -- 5-hour window, weekly window, and the
credit spend control -- not API spend. The OpenAI *platform* Usage & Costs
Admin API is a different thing entirely: it reports token consumption and
dollars for an org with an `sk-admin-...` key, and reads zero for an account
that only uses the ChatGPT subscription.

Unofficial and undocumented, so treat the shape as liable to change: every
field is read defensively and a failure degrades to the last good reading.

Two headers are load-bearing. Without `User-Agent` the edge returns 403, and
`chatgpt-account-id` selects the workspace the allowance belongs to.
"""
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

AUTH_FILE = Path.home() / ".codex" / "auth.json"
USAGE_URL = "https://chatgpt.com/backend-api/codex/usage"

MIN_POLL_SECONDS = 60
_cache = {"at": 0.0, "value": None}
_last_good = {"at": 0.0, "value": None}


def read_tokens():
    """{access_token, account_id} from the Codex auth file, or None."""
    try:
        data = json.loads(AUTH_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    tokens = data.get("tokens")
    if not isinstance(tokens, dict) or not tokens.get("access_token"):
        return None
    return tokens


def _get(tokens):
    request = urllib.request.Request(
        USAGE_URL,
        headers={
            "Authorization": f"Bearer {tokens['access_token']}",
            "Content-Type": "application/json",
            "chatgpt-account-id": tokens.get("account_id") or "",
            # Omitting this User-Agent gets a 403 from the edge, not a 401.
            "User-Agent": "codex_cli_rs/1.0",
            "originator": "codex_cli_rs",
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as e:
        return e.code, None
    except (urllib.error.URLError, OSError, json.JSONDecodeError) as e:
        raise ConnectionError(str(e)) from e


def _clock_label(epoch):
    """'Fri 8:00 PM' -- the device has no clock, so we format it here."""
    if not epoch:
        return None
    local = time.localtime(epoch)
    hour = local.tm_hour % 12 or 12
    ampm = "AM" if local.tm_hour < 12 else "PM"
    return f"{time.strftime('%a', local)} {hour}:{local.tm_min:02d} {ampm}"


def _window(node, now):
    """Normalise one rate-limit window into the shape the display expects."""
    if not isinstance(node, dict):
        return None
    resets_at = node.get("reset_at")
    resets_in = node.get("reset_after_seconds")
    if resets_in is None and resets_at:
        resets_in = max(0, int(resets_at - now))
    try:
        percent = round(float(node.get("used_percent") or 0.0), 1)
    except (TypeError, ValueError):
        return None
    return {
        "percent": percent,
        "resets_in": int(resets_in) if resets_in is not None else None,
        "resets_at": int(resets_at) if resets_at else None,
        "resets_label": _clock_label(resets_at),
    }


def _degraded(reason, now):
    """Prefer a stale-but-real reading over nothing at all."""
    if _last_good["value"]:
        return {**_last_good["value"], "stale_for": int(now - _last_good["at"]), "reason": reason}
    return {"available": False, "reason": reason}


def usage(force=False):
    now = time.time()
    if not force and _cache["value"] and now - _cache["at"] < MIN_POLL_SECONDS:
        return {**_cache["value"], "cached_for": int(now - _cache["at"])}

    tokens = read_tokens()
    if not tokens:
        return _degraded("no Codex credentials -- sign in to the Codex app", now)

    try:
        status, payload = _get(tokens)
    except ConnectionError as e:
        return _degraded(f"network: {e}", now)

    if status in (401, 403):
        return _degraded("Codex token rejected -- open the Codex app", now)
    if status != 200 or not isinstance(payload, dict):
        return _degraded(f"unexpected response (HTTP {status})", now)

    limits = payload.get("rate_limit") or {}
    result = {
        "available": True,
        "plan": payload.get("plan_type"),
        "limit_reached": bool(limits.get("limit_reached")),
    }

    session = _window(limits.get("primary_window"), now)
    week = _window(limits.get("secondary_window"), now)
    if session:
        result["session"] = session
    if week:
        result["week"] = week

    # The credit spend control is the closest analogue to Claude's overage row.
    spend = (payload.get("spend_control") or {}).get("individual_limit")
    credits = _window(spend, now) if isinstance(spend, dict) else None
    if credits:
        try:
            credits["spent"] = round(float(spend.get("used")), 1)
            credits["cap"] = round(float(spend.get("limit")), 1)
            credits["unit"] = spend.get("unit") or "credit"
            # Preformat here so the device never deals in units or currencies.
            credits["value_label"] = f"{credits['spent']:.0f} of {credits['cap']:.0f}"
        except (TypeError, ValueError):
            pass
        result["credits"] = credits

    if not any(k in result for k in ("session", "week", "credits")):
        return _degraded("no rate-limit windows in response", now)

    # No server-side "which limit binds" flag here, so take the tightest.
    worst = max(
        (k for k in ("session", "week", "credits") if k in result),
        key=lambda k: result[k]["percent"],
        default=None,
    )
    result["binding"] = worst

    _cache["at"] = now
    _cache["value"] = result
    _last_good["at"] = now
    _last_good["value"] = result
    return {**result, "cached_for": 0}


if __name__ == "__main__":
    print(json.dumps(usage(force=True), indent=2))
