"""Recent utilization samples, so the display can show trend and projection.

A percentage says where you are; a slope says whether you'll make it to the
reset. Samples live in memory only -- this is a desk gadget, not a database,
and a restart just means the sparkline refills over the next few minutes.
"""
import time
from collections import deque

# One sample per poll (~30s from the device) over ~3 hours.
MAX_SAMPLES = 360

# Ignore slopes computed from too short a baseline: over a few seconds noise
# and rounding dominate and the projection swings wildly.
MIN_SPAN_SECONDS = 120

_series = {}  # window name -> deque[(timestamp, percent)]


def record(window, percent, resets_in=None):
    """Add a sample, dropping the series if the window has rolled over."""
    if percent is None:
        return
    series = _series.setdefault(window, deque(maxlen=MAX_SAMPLES))

    # A reset means the old samples describe a window that no longer exists;
    # keeping them would make the slope look strongly negative.
    if series and percent < series[-1][1] - 5:
        series.clear()

    series.append((time.time(), float(percent)))


def _slope_per_hour(series):
    """Least-squares slope in percent-per-hour over the samples we have."""
    if len(series) < 2:
        return None
    span = series[-1][0] - series[0][0]
    if span < MIN_SPAN_SECONDS:
        return None

    n = len(series)
    t0 = series[0][0]
    xs = [(t - t0) / 3600.0 for t, _ in series]
    ys = [p for _, p in series]
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    denom = sum((x - mean_x) ** 2 for x in xs)
    if denom == 0:
        return None
    return sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denom


def summary(window, percent, resets_in):
    """Trend for one window: slope, sparkline points, and time-to-empty."""
    series = _series.get(window)
    if not series:
        return None

    slope = _slope_per_hour(series)
    out = {
        "samples": len(series),
        "span": int(series[-1][0] - series[0][0]),
        # Downsampled to what a ~190px sparkline can actually show.
        "spark": _spark(series, 48),
    }
    if slope is None:
        return out

    out["per_hour"] = round(slope, 1)

    # Where the current burn rate lands you when the window resets.
    if resets_in and resets_in > 0 and percent is not None:
        projected = percent + slope * (resets_in / 3600.0)
        out["projected"] = round(min(200.0, max(0.0, projected)), 1)
        out["will_exhaust"] = projected >= 100.0

        # Only meaningful while actually climbing toward the cap.
        if slope > 0.5 and percent < 100:
            hours_left = (100.0 - percent) / slope
            out["exhausts_in"] = int(hours_left * 3600)
            # Beating the reset is the good case; say so explicitly.
            out["beats_reset"] = out["exhausts_in"] > resets_in

    return out


def _spark(series, buckets):
    """Percent values thinned to `buckets` points, oldest first."""
    if len(series) <= buckets:
        return [round(p) for _, p in series]
    step = len(series) / buckets
    return [round(series[int(i * step)][1]) for i in range(buckets)]
