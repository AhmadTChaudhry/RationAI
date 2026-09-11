# Brief for an AI agent: set up and flash RationAI

Paste this whole file into a coding agent with shell access (Claude Code,
Codex, or similar) and let it work. It assumes macOS and a LilyGO
T-Display-S3 on USB.

---

You are setting up **RationAI**, a desk display that shows Claude and ChatGPT
plan allowance. Work through this end to end and report what you verified.

## 1. Toolchain

PlatformIO is the only build dependency. Install it if `pio` is missing:

```bash
pipx install platformio   # or: python3 -m pip install --user platformio
```

It lives at `~/.local/bin/pio`. Add that to `PATH` for the session.

## 2. Server

```bash
bash server/install.sh
```

This writes a launchd agent using the current machine's own paths and Python,
then waits for the server to answer. It needs **Claude Code signed in on this
machine**, because the server reads the OAuth token Claude Code already stores in the
Keychain. For the ChatGPT side, the Codex app must be signed in
(`~/.codex/auth.json`).

Verify before moving on:

```bash
curl -s http://127.0.0.1:8787/usage | python3 -m json.tool | head -30
```

You want `"available": true` for at least `claude`. If it says
`token expired`, run `claude -p hi` in a terminal. Only the CLI re-seeds the
Keychain; the desktop app does not. Then re-check.

## 3. WiFi credentials

```bash
cp firmware/src/secrets.h.example firmware/src/secrets.h
```

Ask the user for their WiFi name and password and have **them** put it in that
file. Do not ask them to paste credentials into the chat. `secrets.h` is
gitignored. These are only first-boot defaults; the device's setup portal can
change them later without a reflash.

## 4. Build and flash

```bash
pio run -d firmware -t upload
```

**This board's USB link is unreliable, and a failed flash is recoverable.**
Expect to retry. If it fails:

- `Could not configure port` / `chip stopped responding` mid-write → retry; it
  often succeeds on the second or third attempt.
- **No serial port at all** → the app partition is half-written, and since the
  port is provided by the firmware itself (`ARDUINO_USB_CDC_ON_BOOT=1`), there
  is nothing to enumerate. Tell the user to **hold BOOT, tap RESET, release
  BOOT**. That enters ROM download mode, which always works. You cannot do this
  yourself; it is physical.
- Still failing after a cable swap → the `--no-stub` path is more reliable on
  this chip:

```bash
cd firmware/.pio/build/lilygo-t-display-s3
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port "$(ls /dev/cu.usbmodem*)" --baud 115200 --no-stub \
  --before default_reset --after hard_reset write_flash -z \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

A successful flash ends with `Hash of data verified.` and `Leaving...`.

## 5. Verify it actually works

Do not report success on a build that compiled. Confirm the device fetched
data:

```bash
sleep 30 && grep -c "GET /usage" /tmp/rationai-server.log
```

A rising count from an IP that is not `127.0.0.1` is the display polling. If
the count stays flat, the board is not reaching the server, so check that both
are on the same network.

## 6. Optional: the live avatar

The mascot can animate in step with the user's Claude Code session and wave
when Claude is blocked waiting on them. That needs hooks in
`~/.claude/settings.json` POSTing to the server. **Read the existing file and
merge, never overwriting it.** Add this hook to each of `UserPromptSubmit`,
`PreToolUse`, `PostToolUse`, `Notification`, `Stop`, `SessionStart`,
`SessionEnd`:

```json
{
  "hooks": [{
    "type": "command",
    "command": "curl -s -m 2 -X POST -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:8787/event >/dev/null 2>&1 || true",
    "async": true,
    "timeout": 5
  }]
}
```

`async` and the trailing `|| true` matter: hooks must never block or fail
Claude Code. Validate with `jq -e . ~/.claude/settings.json` afterwards, because a
malformed settings file silently disables every setting in it. The user may
need to restart Claude Code before new hooks take effect.

## Gotchas worth knowing before you start

- **Don't `#include` TFT_eSPI's GFX font headers.** They are pulled in
  automatically when `LOAD_GFXFF` is set and carry no include guards, so a
  second include is a redefinition error.
- **`TFT_RGB_ORDER` must be `TFT_RGB`** on this panel. With `TFT_BGR` every
  colour has red and blue swapped, and coral renders as blue.
- **Pushing the full 320×170 framebuffer costs ~28ms.** The code deliberately
  redraws only what changed. Don't "simplify" it into a full-frame redraw.
- **The usable panel width is 166px.** Anything wider clips silently.
- Never put a token or API key in the firmware. The board reads JSON off the
  LAN; that is the whole security model.
