"""What Claude is doing *right now*, for the device's live channel.

Two independent sources, because they answer different questions:

1. Hook events POSTed by Claude Code (see /event in usage_server.py) say what
   state the session is in -- thinking, running a tool, or blocked waiting on
   you. That last one is the point: the board can tell you Claude needs input
   without you switching windows.

2. The session JSONL gives token burn and cost. Claude Code appends a usage
   record per assistant turn, so tailing the newest file yields a live rate.

Quota polling is once every 30s; this is polled every second or two, so
everything here is cheap: no network, no API calls, incremental file reads.
"""
import json
import time
from pathlib import Path

CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"

# How long after the last hook event we still trust the reported state.
STATE_TTL = 900
# Window for the tokens/sec figure.
BURN_WINDOW = 60

# USD per million tokens. Used only for the session-cost odometer, which is an
# estimate by construction -- the authoritative number is the credits row.
PRICING = {
    "opus": {"in": 5.00, "out": 25.00, "cache_read": 0.50, "cache_write": 6.25},
    "sonnet": {"in": 2.00, "out": 10.00, "cache_read": 0.20, "cache_write": 2.50},
    "haiku": {"in": 1.00, "out": 5.00, "cache_read": 0.10, "cache_write": 1.25},
    "fable": {"in": 10.00, "out": 50.00, "cache_read": 1.00, "cache_write": 12.50},
}

# Last hook event seen.
_state = {"state": "idle", "tool": None, "at": 0.0, "detail": None}

# Incremental read position for the file we're tailing. `seen` dedupes: Claude
# Code writes each assistant turn to the log more than once, which would
# otherwise double both the burn rate and the session cost.
_tail = {"path": None, "offset": 0, "samples": [], "cost": 0.0, "turns": 0, "seen": set()}


def record_event(payload):
    """Fold one Claude Code hook event into the current state.

    Payload comes from a hook, so treat it as data: only the event name and
    tool name are read, and only against a known set.
    """
    event = str(payload.get("hook_event_name") or payload.get("event") or "")
    tool = payload.get("tool_name") or payload.get("tool")
    now = time.time()

    if event in ("UserPromptSubmit", "SessionStart"):
        _state.update(state="working", tool=None, at=now, detail=None)
    elif event == "PreToolUse":
        _state.update(state="tool", tool=str(tool)[:32] if tool else None, at=now)
    elif event == "PostToolUse":
        _state.update(state="working", tool=str(tool)[:32] if tool else None, at=now)
    elif event == "Notification":
        # Claude is blocked on the human -- permission, or a question.
        _state.update(state="waiting", tool=None, at=now, detail="needs you")
    elif event == "Stop":
        _state.update(state="waiting", tool=None, at=now, detail="turn finished")
    elif event == "SessionEnd":
        _state.update(state="idle", tool=None, at=now, detail=None)
    else:
        return False
    return True


def _parse_ts(value):
    """Record timestamps, so replayed history doesn't look like it happened now."""
    if not value:
        return None
    try:
        from datetime import datetime
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (ValueError, AttributeError):
        return None


def _newest_session_file():
    newest = None
    if not CLAUDE_PROJECTS.is_dir():
        return None
    for path in CLAUDE_PROJECTS.rglob("*.jsonl"):
        try:
            mtime = path.stat().st_mtime
        except OSError:
            continue
        if newest is None or mtime > newest[0]:
            newest = (mtime, path)
    return newest[1] if newest else None


def _price(model, usage):
    key = next((k for k in PRICING if k in (model or "")), None)
    if not key:
        return 0.0
    p = PRICING[key]
    return (
        usage.get("input_tokens", 0) * p["in"]
        + usage.get("output_tokens", 0) * p["out"]
        + usage.get("cache_read_input_tokens", 0) * p["cache_read"]
        + usage.get("cache_creation_input_tokens", 0) * p["cache_write"]
    ) / 1_000_000


def _tail_session():
    """Read whatever is new in the active session file."""
    path = _newest_session_file()
    if not path:
        return

    # A new session means a new odometer.
    if _tail["path"] != path:
        _tail.update(path=path, offset=0, samples=[], cost=0.0, turns=0, seen=set())

    try:
        size = path.stat().st_size
        if size < _tail["offset"]:
            _tail["offset"] = 0  # truncated or rotated
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            fh.seek(_tail["offset"])
            for line in fh:
                if '"usage"' not in line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                msg = rec.get("message")
                if not isinstance(msg, dict):
                    continue
                usage = msg.get("usage")
                if not isinstance(usage, dict):
                    continue
                key = f"{msg.get('id')}:{rec.get('requestId')}"
                if key in _tail["seen"]:
                    continue
                _tail["seen"].add(key)
                total = (
                    usage.get("input_tokens", 0)
                    + usage.get("output_tokens", 0)
                    + usage.get("cache_creation_input_tokens", 0)
                    + usage.get("cache_read_input_tokens", 0)
                )
                # The record's own timestamp, not now: reading the backlog on
                # startup must not register as a burst of current activity.
                _tail["samples"].append((_parse_ts(rec.get("timestamp")) or time.time(), total))
                _tail["cost"] += _price(msg.get("model"), usage)
                _tail["turns"] += 1
            _tail["offset"] = fh.tell()
    except OSError:
        return

    cutoff = time.time() - BURN_WINDOW
    _tail["samples"] = [s for s in _tail["samples"] if s[0] >= cutoff]


def snapshot():
    """The live channel: small, cheap, safe to poll every second."""
    _tail_session()
    now = time.time()

    state = _state["state"]
    age = now - _state["at"] if _state["at"] else None
    # Without hooks installed, or after a long silence, don't claim to know.
    if _state["at"] == 0:
        state = "unknown"
    elif age is not None and age > STATE_TTL:
        state = "idle"

    burned = sum(n for _, n in _tail["samples"])
    return {
        "state": state,
        "tool": _state["tool"],
        "detail": _state["detail"],
        "since": int(age) if age is not None else None,
        "tokens_per_sec": round(burned / BURN_WINDOW, 1),
        "session_cost": round(_tail["cost"], 2),
        "session_turns": _tail["turns"],
    }


if __name__ == "__main__":
    print(json.dumps(snapshot(), indent=2))
