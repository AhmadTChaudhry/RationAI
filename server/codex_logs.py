"""Fallback: the last rate_limits snapshot Codex CLI wrote to its session log.

Superseded by codex_live.py, which reads the real allowance from the ChatGPT
backend. Kept because it needs no network and no valid token -- but it is only
as fresh as your last Codex session, so a window whose reset has passed is
reported as rolled over rather than replayed as fact.
"""
import json
import time
from datetime import datetime, timezone
from pathlib import Path

CODEX_DIR = Path.home() / ".codex" / "sessions"


def _recent_files(root, max_age, limit=None):
    if not root.is_dir():
        return []
    cutoff = time.time() - max_age
    found = []
    for path in root.rglob("*.jsonl"):
        try:
            mtime = path.stat().st_mtime
        except OSError:
            continue
        if mtime >= cutoff:
            found.append((mtime, path))
    found.sort(reverse=True)
    if limit:
        found = found[:limit]
    return [p for _, p in found]


def _parse_ts(value):
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (ValueError, AttributeError):
        return None


def _find_key(node, key):
    """Codex nests rate_limits at varying depths; walk until we find it."""
    if isinstance(node, dict):
        if isinstance(node.get(key), dict):
            return node[key]
        for value in node.values():
            hit = _find_key(value, key)
            if hit:
                return hit
    elif isinstance(node, list):
        for value in node:
            hit = _find_key(value, key)
            if hit:
                return hit
    return None


def usage():
    """Latest quota snapshot Codex CLI wrote to its session log.

    Unlike the Claude probe this is passive: it is only as fresh as the last
    time you actually ran Codex, which `stale_for` reports.
    """
    newest = None
    for path in _recent_files(CODEX_DIR, 30 * 24 * 3600, limit=60):
        try:
            with path.open(encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    if '"rate_limits"' not in line:
                        continue
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    limits = _find_key(rec, "rate_limits")
                    if not limits or not limits.get("primary"):
                        continue
                    ts = _parse_ts(rec.get("timestamp")) or 0
                    if newest is None or ts > newest[0]:
                        newest = (ts, limits)
        except OSError:
            continue
        if newest:
            break  # files come newest-first, so the first hit is the freshest

    if not newest:
        return {"available": False, "reason": "no Codex quota data on this machine"}

    ts, limits = newest
    now = time.time()
    out = {
        "available": True,
        "plan": limits.get("plan_type"),
        "as_of": int(ts),
        "stale_for": int(now - ts),
    }
    for src, name in (("primary", "session"), ("secondary", "week")):
        window = limits.get(src)
        if not isinstance(window, dict):
            continue
        resets_at = window.get("resets_at") or 0
        percent = round(float(window.get("used_percent") or 0.0), 1)

        # A snapshot only describes the window it was taken in. Once that
        # window's reset time has passed the recorded number is not merely
        # stale, it is wrong -- and since every Codex turn writes a fresh
        # rate_limits line, having no newer snapshot means no usage since.
        rolled_over = bool(resets_at) and resets_at < now
        if rolled_over:
            percent = 0.0

        out[name] = {
            "percent": percent,
            "window_minutes": window.get("window_minutes"),
            "resets_in": max(0, int(resets_at - now)) if resets_at and not rolled_over else None,
            "rolled_over": rolled_over,
        }
    return out


def demo_claude(elapsed):
    """Synthetic fast-burn account, for checking the display without waiting
    for a real one. Climbs the 5-hour window ~35%/h so the projection warning
    and the sparkline both have something to show."""
    percent = min(99.0, 18.0 + elapsed / 3600.0 * 35.0)
    resets_in = max(60, int(3.6 * 3600 - elapsed))
    return {
        "available": True,
        "account": "demo",
        "binding": "session",
        "session": {"percent": round(percent, 1), "resets_in": resets_in,
                    "resets_label": claude_live._clock_label(time.time() + resets_in)},
        "week": {"percent": 40.0, "resets_in": 97000,
                 "resets_label": claude_live._clock_label(time.time() + 97000)},
        "credits": {"percent": 59.0, "resets_in": 1788000, "spent": 41.3, "cap": 70.0,
                    "resets_label": claude_live._clock_label(time.time() + 1788000)},
    }


DEMO = False
_started = time.time()
