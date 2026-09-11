# RationAI user manual

Claude and ChatGPT usage limits on a LilyGO T-Display-S3, with Clawd as a live
avatar of your Claude Code session.

---

## The screen

```
┌──────────────────────────────────────────┬──────────────────┐
│ YOUR USAGE LIMITS                        │ CLAUDE  auto  87%│  ← brand · mode · battery
│ 5-hour limit                        23%  │                  │
│ ██████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  │      (\_/)       │  ← Clawd, animated
│                      Resets in 4h19m     │     ( •,•)       │
│ Weekly · all models                 46%  │     (")_(")      │
│ ███████████░░░░░░░░░░░░░░░░░░░░░░░░░░░  │                  │
│                      Resets Fri 8:00 PM  │    needs you     │  ← what Claude is doing
│ Usage credits              A$41 of A$70  │  ╱╲__╱╲_         │  ← trend sparkline
│ ████████████████████░░░░░░░░░░░░░░░░░░  │    +3.4%/h       │  ← burn rate
│                      Resets Thu 10:00 AM │                  │
│ 192.168.88.5                             │      $12.40      │  ← session cost
└──────────────────────────────────────────┴──────────────────┘
```

### The three bars

Bars **fill as you spend**, matching Claude Code's own `/usage` panel. The
unfilled part is what's left.

| Row | Claude | ChatGPT |
| --- | --- | --- |
| 5-hour limit | rolling 5h window | `primary_window` |
| Weekly | all models, 7 days | `secondary_window` |
| Usage credits | spend, in your real currency | credit spend control |

Colours: brand colour under 75% used, **amber** at 75%, **red** at 90%.

The **binding limit**, the one actually constraining you right now, has its
value in the brand colour; the others stay grey. Claude reports this itself;
for ChatGPT it's whichever window is tightest.

### Reset times

The 5-hour row counts down live (`Resets in 4h19m`), ticking on the device
between polls. The longer rows show wall-clock time (`Resets Fri 8:00 PM`),
formatted by the server, because the board has no clock and no timezone.

If the projection says you'll hit the cap *before* the window resets, an amber
`· cap in 2h16m` appears next to the reset time.

### Right column

- **Sparkline**: the binding window over the last ~3h, on a fixed 0-100 scale
  (an autoscaled trace makes a flat 20→22% hour look like a cliff)
- **`+3.4%/h`**: burn rate for that window
- **`$12.40`**: estimated cost of the current Claude Code session

---

## Clawd

The mascot's animation reflects **what Claude Code is doing right now**, from
hooks, and falls back to your quota level when the session is quiet.

### Live session (needs hooks installed)

| Claude is… | Clawd | Label |
| --- | --- | --- |
| blocked on you (permission, question) | **waving** | `needs you` (amber) |
| finished its turn | waving | `turn finished` |
| running Bash | racing car | `Bash` |
| searching (Grep, Glob, WebSearch, WebFetch) | magnifier | tool name |
| thinking, or editing | laptop | `thinking` |

**The wave is the point.** You don't have to switch windows to find out whether
Claude is waiting on you.

In **CHATGPT mode the mascot turns evil**, with a green body, red eyes and angled
brows, generated from the same frames rather than separate artwork.

### Quota fallback (no hooks, or session idle >20s)

| Left on the tightest limit | Clawd |
| --- | --- |
| 75%+ | jumping happy |
| 50%+ | dancing |
| 25%+ | laptop |
| 10%+ | pointing |
| under 10% | lurking |
| no data | cloud |

---

## Buttons

Two buttons: **KEY** and **BOOT** (BOOT is the one beside the USB-C port).

| Gesture | Effect |
| --- | --- |
| **KEY** tap | next animation |
| **KEY** tap twice | switch landscape / portrait |
| **BOOT** tap | previous animation |
| **BOOT** tap twice | switch Claude / ChatGPT |
| **hold either** | back to the live view, and refresh now |
| **KEY** held while powering on | open the WiFi setup portal |

Brand and layout are remembered across reboots. A double tap resolves shortly
after the second press, so tap at whatever pace feels natural.

### What changes by itself, and what doesn't

**The brand never changes on its own.** CLAUDE or CHATGPT stays exactly where
you put it until you tap BOOT twice again, and it is remembered across
reboots. So is the portrait/landscape choice.

Nothing else moves it either: threshold alerts flash the screen but leave the
displayed brand alone.

**The animation does change by itself**. That's the avatar. It follows your
Claude Code session (or your quota level when the session is quiet). Tap a
button to pin one; hold either button to hand it back.

Note the live avatar reports **Claude Code only**, so in CHATGPT mode the
mascot falls back to quota mood rather than narrating a Claude session under a
ChatGPT header.

---

## Portrait totem

Two KEY taps flips to the panel's native 170×320 portrait layout, a
different arrangement rather than a rotation:

```
┌──────────────────┐
│ RATION AI   [⚡██]│
│                  │
│      (\_/)       │
│     ( •,•)       │  Clawd, scaled to fill
│     (")_(")      │  the top half and centred
│                  │  (up to 6x pixel scale)
│                  │
│      CLAUDE      │  wordmark
│    needs you     │  live state
│                  │
│ 5-hour      23%  │  bigger type
│ ████░░░░░░░░░░░░ │
│ Weekly      46%  │
│ ███████░░░░░░░░░ │
│ Credits  A$41/70 │
│ ██████████░░░░░░ │
└──────────────────┘
```

The avatar is scaled to fill the area and centred, so it stays large whatever
animation is playing. There's no trend chart in this layout; the three limits
get the room instead.

Stand it on its short edge. The choice is remembered across reboots.

## Wordmark typeface

The portrait wordmark uses **Supercharge Condensed 18pt**, converted from the
OTF with Adafruit's `fontconvert`. Everything else, including limit labels, percentages and
reset times, stays on TFT_eSPI's built-in bitmap fonts, which are crisper
below ~9pt.

To change it, edit the single `setFreeFont` call in `drawBrandWordPortrait()`.
Two constraints: the usable width is **166px** and the wordmark slot is
**44px** tall, measured against `CHATGPT`, the longer of the two words.

| Face | CHATGPT | |
| --- | --- | --- |
| **Supercharge Condensed 18pt** | 158×40 | in use |
| SF Pro Display / SF Rounded 18pt | ~157×25 | fits easily |
| FreeMono 18pt (bundled) | 147×26 | most headroom |
| FreeSansOblique 18pt (bundled) | 165×25 | 1px spare |
| Supercharge 18pt (regular) | 195×40 | clips |

Don't `#include` the bundled GFX font headers. TFT_eSPI already pulls them all
in, and Adafruit's headers have no include guards, so a second include is a
redefinition error.

## Battery

Top right, in both layouts: a real icon with proportional fill, red at 15% or
below, and a **⚡ bolt** while charging.

For a 3.7V LiPo: empty at 3.30V, full at 4.20V.

**Charging is inferred, not read.** This board breaks out no charge-status pin.
Plugged in, the charger drives the sense rail to roughly USB voltage, well
above a cell's 4.2V full mark, so anything at **4.30V or higher** means
external power, and the bolt shows. Unplugged, a sustained rise of ≥20mV over
~30s also counts as charging. The icon only hides below 2.5V, when nothing is
on the sense pin at all.

While the charger is driving the rail the cell's real state of charge isn't
measurable, so the icon shows full with a bolt rather than guessing.

## Alerts

- Crossing **75%** flashes amber; crossing **90%** flashes red, and the panel
  jumps to the brand that needs attention
- A window **rolling over** flashes the brand colour (good news)
- Backlight sits at ~47% while everything is under 75%, and goes full
  brightness when something wants you

---

## Using it with a second laptop

The display doesn't need to know which machine it's talking to. Each server
advertises itself on the network as `_rationai._tcp`, and the board browses
for that service, so it connects to whichever laptop is running the server,
on whatever network, with nothing configured.

**On the second laptop, once:**

```bash
bash server/install.sh
```

That writes a launchd agent using that machine's own paths and Python, so the
same repo works under a different username. It needs Claude Code (and Codex,
if you want that side) signed in there, since the tokens are what the server reads.

**On the board, once per network:** hold KEY while powering on, join
`RationAI-Setup`, pick the WiFi. It remembers **four** networks (home, work,
phone hotspot, spare) and tries them all at boot, so adding one never forgets
the last. Leave the server host field blank; discovery handles it.

### Corporate WiFi will probably not work

Most workplace networks block client-to-client traffic and multicast. That
kills both the discovery and the HTTP request itself, and no amount of
configuration on the board fixes it. The network is refusing to route
between your laptop and the display.

**Use your phone's hotspot at work.** Join the board and the laptop to it
once; the board remembers it, and a hotspot doesn't isolate its clients.
That's the reliable path, not a workaround.

### Falling back to a fixed address

If discovery can't work (multicast blocked but direct traffic allowed), put
the laptop's IP in the portal's server host field. A bare IP skips mDNS
entirely. The board tries, in order: service discovery → the stored host over
mDNS → the literal address.

## Setup

### First boot / changing WiFi

Hold **KEY** while powering on, then join the WiFi network **`RationAI-Setup`**
from your phone or laptop. The portal sets:

- WiFi network and password
- **Server host**: your Mac's name (`scutil --get LocalHostName`, no `.local`),
  or a bare IP to skip mDNS entirely
- **Server port**: default `8787`

These are stored on the device, so changing networks never needs a reflash.

### The server

```bash
python3 server/usage_server.py
```

Runs permanently via launchd:

```bash
bash server/install.sh
```

After editing server code, launchd won't notice on its own:

```bash
launchctl kickstart -k gui/$(id -u)/com.rationai.server
```

### Hooks (for the live avatar)

Already installed in `~/.claude/settings.json` on seven events, `async` with a
2s timeout so they can never slow Claude Code down. Claude Code may need
`/hooks` opened once, or a restart, before it picks them up.

Test with:

```bash
claude -p "run: echo hello"
```

Clawd should walk through thinking → Bash → waving.

---

## Status line

Bottom left shows the board's IP when healthy, or the problem when not:

| Message | Meaning |
| --- | --- |
| `192.168.88.5` | fine, that's the board's address |
| `stale 12m` | server is serving its last real reading; a token expired |
| `token expired 4m ago …` | run any Claude Code command to renew it |
| `can't find <host>` | mDNS failed and no fallback is set, check the portal |
| `mDNS failed, using fallback` | working, via the literal IP |
| `http 500` / `json: …` | server reachable but unhappy, check its log |
| `wifi lost` | reconnecting |

---

## Flashing

```bash
~/.local/bin/pio run -d firmware -t upload
```

**If it fails**, this board's USB link is marginal. A partly-written app means
no serial port at all, because the port is provided by the firmware itself
(`ARDUINO_USB_CDC_ON_BOOT=1`). Recovery:

1. **Hold BOOT → tap RESET → release BOOT** (ROM download mode, always works)
2. Retry, as it often takes two or three attempts
3. Use a known-good data cable, plugged **directly** into the Mac

The `--no-stub` path is more reliable here than PlatformIO's default:

```bash
cd firmware/.pio/build/lilygo-t-display-s3 && ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/cu.usbmodem101 --baud 115200 --no-stub --before default_reset --after hard_reset write_flash -z 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

---

## Known limits

**Both data sources are undocumented.** Claude's `/api/oauth/usage` and
ChatGPT's `/backend-api/codex/usage` are what the official clients use, not
published APIs. They can change without notice. Both readers degrade to the
last real reading tagged `stale` rather than showing nothing.

**Claude's token expires.** The server reuses the token Claude Code stores and
deliberately never refreshes it, because refresh tokens rotate, and renewing behind
Claude Code's back could log you out of the CLI. So if the CLI sits idle long
enough, the token lapses and the panel goes stale until you run any Claude Code
command.

**Session cost is an estimate.** Computed from local logs against a pricing
table in `server/activity.py`. The authoritative number is the credits row.

**Usage credits are Claude's real currency** (AUD here, not USD). The endpoint
reports the currency and the server formats it.
