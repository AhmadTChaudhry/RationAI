// RationAI -- Claude and ChatGPT plan allowance on a LilyGO T-Display-S3.
//
// The board holds no credentials: it polls a small JSON endpoint served by
// server/usage_server.py on the Mac, and draws whatever comes back. Both
// brands' allowances arrive in every poll and are held in memory, so switching
// modes is instant and costs no request.
//
// Layout mirrors Claude Code's own /usage panel: three limit bars down the
// left, an animated mascot in a column on the right.
//
// Drawing is split by update rate, because pushing the whole 320x170 panel
// costs ~28ms on this 8-bit parallel bus -- far too slow to do every frame.
// The mascot lives in a small sprite pushed each frame (~4ms); the bars and
// text are drawn straight to the panel only when their values change.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>

// The wordmark uses Supercharge Condensed, converted from the OTF with
// Adafruit's fontconvert. Small readouts stay on TFT_eSPI's built-in bitmap
// fonts, which are crisper at 8px.
// Included once and only here -- like Adafruit's own font headers it carries
// no include guard.
#include "SuperchargeCn18.h"
#include "secrets.h"
#include "splash_animations.h"

// T-Display-S3 must assert this or the panel stays dark off USB.
static const int PIN_POWER_ON = 15;
// Both buttons are active-low with pull-ups. GPIO0 is also the strapping pin,
// so holding it through a reset still enters download mode -- that's the
// flashing behaviour, unaffected by reading it at runtime.
static const int PIN_KEY = 14;   // tap: next animation, x2: switch layout
static const int PIN_BOOT = 0;   // tap: prev animation, x2: switch brand
static const uint32_t LONG_PRESS_MS = 700;
static const uint32_t DEBOUNCE_MS = 25;
static const uint32_t MULTI_PRESS_MS = 400;  // idle time that ends a press burst

static const uint32_t POLL_INTERVAL_MS = 30000;
// The live channel is local-only on the server (no API calls), so it can be
// polled fast enough for the mascot to track what Claude is actually doing.
static const uint32_t LIVE_INTERVAL_MS = 1500;
static const uint32_t HTTP_TIMEOUT_MS = 8000;
static const uint32_t FRAME_MS = 33;  // ~30fps

// Left column: the limit bars.
static const int PAD = 8;
static const int BAR_W = 194;
static const int BAR_H = 8;
static const int COL_RIGHT_EDGE = PAD + BAR_W;  // 202
static const int DIVIDER_X = 207;
static const int ROW_TOP[3] = {20, 62, 104};

// Right column: the mascot. Art is authored on a shared 55x37 cell grid.
static const int STAGE_W = 55, STAGE_H = 37;
static const int CELL = 2;
static const int STAGE_PX_W = STAGE_W * CELL;  // 110
static const int STAGE_PX_H = STAGE_H * CELL;  // 74
static const int STAGE_X = 210;
static const int STAGE_Y = 18;
static const int CX_RIGHT = STAGE_X + STAGE_PX_W / 2;

// Portrait ("totem") layout: the panel's native 170x320. Not a rotation of the
// landscape layout -- a different arrangement, built around a 3x Clawd.
static const int P_W = 170, P_H = 320;
static const int P_PAD = 8;
static const int P_BAR_W = 154;
static const int P_STAGE_X = 2, P_STAGE_Y = 14;
static const int P_STAGE_W = 166, P_STAGE_H = 134;  // the avatar gets the top half
static const int P_CX = P_W / 2;
static const int P_BRAND_Y = 152;
static const int P_LIVE_Y = 198;
static const int P_ROW_TOP[3] = {224, 256, 288};
// Short labels: at font 2 the landscape wording would eat the whole width.
static const char *P_ROW_LABEL[3] = {"5-hour", "Weekly", "Credits"};

// Live geometry, switched with the orientation. The stage is now an *area*
// rather than a fixed cell size: each animation is scaled to fill it and
// centred, because the art's bounding boxes vary wildly (13x17 to 50x33) and
// positioning them on the shared 55x37 grid left most of the space empty --
// "walking" wasted 57px above itself and drew at 72x54 inside 165x111.
static bool portrait = false;
static int stageX = STAGE_X, stageY = STAGE_Y;
static int stageAreaW = STAGE_PX_W, stageAreaH = STAGE_PX_H;
static const int MAX_CELL = 6;  // beyond this the pixels read as mush
static const int SPARK_X = 212;
static const int SPARK_Y = 108;
static const int SPARK_W = 106;
static const int SPARK_H = 22;

// Battery sense is a 2:1 divider on GPIO4. With no cell attached the reading
// sits near the USB rail, so anything implausible is treated as "no battery"
// and the pip is hidden rather than faked.
static const int PIN_BATTERY = 4;
static const float BATT_EMPTY_V = 3.30f;
static const float BATT_FULL_V = 4.20f;

static const uint8_t BL_BRIGHT = 255;
static const uint8_t BL_DIM = 120;  // nothing urgent -- stop shouting

static TFT_eSPI tft;
static TFT_eSprite stage(&tft);  // just the mascot: small enough per frame

// All drawing goes through this. Normally the panel; briefly a full-screen
// sprite while capturing a screenshot, because reading pixels back off this
// ST7789 over the parallel bus returns mangled colour.
static TFT_eSPI *g = &tft;
static TFT_eSprite shotBuf(&tft);
static bool capturing = false;

static uint16_t COL_BG, COL_TEXT, COL_DIM, COL_TRACK;
static uint16_t COL_CLAUDE, COL_CHATGPT, COL_WARN, COL_DANGER;
static uint16_t COL_EVIL_EYE;

enum Brand { BRAND_CLAUDE = 0, BRAND_CHATGPT = 1, BRAND_COUNT = 2 };
static const char *BRAND_KEY[BRAND_COUNT] = {"claude", "codex"};
static const char *BRAND_LABEL[BRAND_COUNT] = {"CLAUDE", "CHATGPT"};
// The displayed brand only ever changes by a button press, and is remembered
// across reboots.
static int brand = BRAND_CLAUDE;
// Thresholds an alert fires on, ascending. Crossing one upward flashes.
static const float ALERT_LEVELS[2] = {75.0f, 90.0f};

// One row of the panel: a limit window.
struct Limit {
  bool present = false;
  float percent = 0;   // percent of the window *used*
  long resetsIn = -1;  // seconds at fetch time, -1 = unknown
  String resetLabel;   // "Fri 8:00 PM", from the server's clock
  String value;        // "23%", "$41 of $70", "1223 of 1600"
  bool binding = false;

  bool hasTrend = false;
  float perHour = 0;
  long exhaustsIn = -1;
  bool willExhaust = false;
  uint8_t spark[48] = {0};
  int sparkN = 0;

  float shown = 0;  // animated toward percent
};

struct BrandState {
  bool available = false;
  String note = "starting up";
  long staleFor = -1;
  Limit limits[3];
};

static const char *ROW_LABEL[3] = {"5-hour limit", "Weekly limit", "Usage credits"};
static BrandState brands[BRAND_COUNT];

// Server host/port live in NVS so they can be changed from the setup portal
// instead of by reflashing -- which on this board is the riskiest operation
// there is. secrets.h only supplies the first-boot defaults.
static Preferences prefs;
static String cfgHost = USAGE_HOST;
static int cfgPort = USAGE_PORT;

// Up to four remembered networks -- home, work, phone hotspot, spare. The
// ESP32 itself only keeps the single most recent set of station credentials,
// so joining a new network would otherwise forget the last one.
static const int MAX_NETS = 4;
static WiFiMulti wifiMulti;

// Keep the newest first so a full list drops the least recently used.
static void rememberNetwork(const String &ssid, const String &pass) {
  if (ssid.isEmpty()) return;

  String ssids[MAX_NETS], passes[MAX_NETS];
  int n = 0;
  ssids[n] = ssid;
  passes[n] = pass;
  n++;
  for (int i = 0; i < MAX_NETS && n < MAX_NETS; i++) {
    String s = prefs.getString(("net" + String(i) + "s").c_str(), "");
    if (s.isEmpty() || s == ssid) continue;  // drop the duplicate
    ssids[n] = s;
    passes[n] = prefs.getString(("net" + String(i) + "p").c_str(), "");
    n++;
  }
  for (int i = 0; i < MAX_NETS; i++) {
    prefs.putString(("net" + String(i) + "s").c_str(), i < n ? ssids[i] : String(""));
    prefs.putString(("net" + String(i) + "p").c_str(), i < n ? passes[i] : String(""));
  }
}

// Try every remembered network at once and take whichever answers.
static bool connectRemembered(uint32_t timeoutMs) {
  int added = 0;
  for (int i = 0; i < MAX_NETS; i++) {
    String ssid = prefs.getString(("net" + String(i) + "s").c_str(), "");
    if (ssid.isEmpty()) continue;
    wifiMulti.addAP(ssid.c_str(), prefs.getString(("net" + String(i) + "p").c_str(), "").c_str());
    added++;
  }
  if (!added) return false;
  return wifiMulti.run(timeoutMs) == WL_CONNECTED;
}

// What Claude Code is doing right now, from the server's /live channel.
struct Live {
  bool valid = false;
  String state = "unknown";   // working | tool | waiting | idle | unknown
  String tool;
  String detail;
  float tokensPerSec = 0;
  float sessionCost = 0;
  uint32_t receivedAt = 0;
};
static Live live;

static String statusLine = "starting up";
static String usageUrl;  // resolved over mDNS, cleared on failure
static uint32_t lastPollMs = 0;
static uint32_t fetchedAtMs = 0;
static bool everFetched = false;
static uint8_t backlight = BL_BRIGHT;

// What the panel currently shows, so each element repaints only when its own
// value moves. All declared together because drawChrome() invalidates them.
static bool chromeDrawn = false;
static String drawnStatus;
static String drawnAnimLabel;
static int drawnSparkSig = -1;
static int drawnBattery = -999;
static String drawnCost;
static String drawnBrandWord;
static int drawnPercent[3] = {-999, -999, -999};
static String drawnValue[3], drawnReset[3];

// Worst utilisation across a brand's windows -- the number that decides both
// the mascot's mood and which brand auto mode shows.
static float worstUsed(const BrandState &b) {
  if (!b.available) return -1;
  float worst = 0;
  for (const Limit &l : b.limits) {
    if (l.present && l.percent > worst) worst = l.percent;
  }
  return worst;
}

static BrandState &current() { return brands[brand]; }
static uint16_t brandColour() { return brand == BRAND_CLAUDE ? COL_CLAUDE : COL_CHATGPT; }

// ---------------------------------------------------------------- formatting

static String humanDuration(long seconds) {
  if (seconds < 0) return "--";
  if (seconds < 60) return "now";
  long minutes = seconds / 60;
  if (minutes < 60) return String(minutes) + "m";
  long hours = minutes / 60;
  minutes %= 60;
  if (hours < 24) return String(hours) + "h" + String(minutes) + "m";
  return String(hours / 24) + "d" + String(hours % 24) + "h";
}

// Counted down locally between polls so the number keeps ticking.
static long liveResetsIn(const Limit &l) {
  if (l.resetsIn < 0) return -1;
  long elapsed = (long)((millis() - fetchedAtMs) / 1000);
  long remaining = l.resetsIn - elapsed;
  return remaining > 0 ? remaining : 0;
}

// Bars fill as you spend, like the panel they're copied from.
static uint16_t barColour(float used) {
  if (used >= 90) return COL_DANGER;
  if (used >= 75) return COL_WARN;
  return brandColour();
}

// ----------------------------------------------------------------- battery

static float batteryVolts() {
  uint32_t total = 0;
  for (int i = 0; i < 8; i++) total += analogReadMilliVolts(PIN_BATTERY);
  return (total / 8.0f) * 2.0f / 1000.0f;
}

// A lithium cell's voltage is not linear in charge: it sits near 3.7-3.8V for
// most of the discharge and then falls away quickly. A straight line from 3.3V
// to 4.2V reads roughly 20 points high through the middle, so interpolate a
// curve instead. Approximate, and it reads low under load.
static const struct { float v; uint8_t pct; } BATT_CURVE[] = {
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.93f, 70}, {3.87f, 60},
    {3.82f, 50},  {3.78f, 40}, {3.75f, 30}, {3.71f, 20}, {3.66f, 10},
    {3.55f, 5},   {BATT_EMPTY_V, 0},
};

static int batteryPercent(float volts) {
  if (volts >= BATT_CURVE[0].v) return 100;
  for (size_t i = 1; i < sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]); i++) {
    if (volts >= BATT_CURVE[i].v) {
      const float span = BATT_CURVE[i - 1].v - BATT_CURVE[i].v;
      const float frac = span > 0 ? (volts - BATT_CURVE[i].v) / span : 0;
      return (int)roundf(BATT_CURVE[i].pct +
                         frac * (BATT_CURVE[i - 1].pct - BATT_CURVE[i].pct));
    }
  }
  return 0;
}

// This board exposes no charge-status pin, so charging is inferred, not read:
// a 3.7V cell rests at 4.2V full, so anything above that means the charger is
// driving the rail -- and below it, a sustained rise over ~30s says the same.
static const float BATT_CHARGING_V = 4.24f;
static bool batteryCharging(float volts) {
  if (volts >= BATT_CHARGING_V) return true;

  static float ring[16] = {0};
  static uint8_t head = 0, filled = 0;
  static uint32_t lastSampleMs = 0;
  if (millis() - lastSampleMs >= 2000) {  // ~32s of history across the ring
    lastSampleMs = millis();
    ring[head] = volts;
    head = (head + 1) % 16;
    if (filled < 16) filled++;
  }
  if (filled < 16) return false;
  float oldest = ring[head];  // head now points at the oldest entry
  return volts > oldest + 0.02f;
}

// A real icon rather than a bare number: body, terminal nub, proportional
// fill, and a bolt when charging. Drawn at its top-left corner.
static void drawBatteryIcon(int x, int y, int pct, bool charging) {
  const int w = 22, h = 11;
  uint16_t frame = pct <= 15 && !charging ? COL_DANGER : COL_DIM;

  g->drawRoundRect(x, y, w, h, 2, frame);
  g->fillRect(x + w, y + 3, 2, h - 6, frame);  // terminal nub

  int inner = w - 4;
  int fill = (int)roundf(inner * constrain(pct, 0, 100) / 100.0f);
  if (fill > 0) {
    uint16_t bar = charging ? COL_CHATGPT : (pct <= 15 ? COL_DANGER : COL_DIM);
    g->fillRect(x + 2, y + 2, fill, h - 4, bar);
  }

  if (charging) {
    // Small lightning bolt, drawn over the fill so it reads at any level.
    int cx = x + w / 2, cy = y + h / 2;
    g->drawLine(cx + 2, cy - 4, cx - 2, cy, COL_BG);
    g->drawLine(cx - 2, cy, cx + 1, cy, COL_BG);
    g->drawLine(cx + 1, cy, cx - 2, cy + 4, COL_BG);
    g->drawLine(cx + 3, cy - 4, cx - 1, cy, COL_TEXT);
    g->drawLine(cx - 1, cy, cx + 2, cy, COL_TEXT);
    g->drawLine(cx + 2, cy, cx - 1, cy + 4, COL_TEXT);
  }
}

// ----------------------------------------------------------------- buttons

// Debounced button with short/long press. Flags latch until read so a press
// can't be missed between frames.
struct Button {
  uint8_t pin;
  bool stable = true;
  bool lastRaw = true;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool longFired = false;
  bool shortPress = false;
  bool longPress = false;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
  }

  void update() {
    bool raw = digitalRead(pin);
    uint32_t now = millis();
    if (raw != lastRaw) {
      lastRaw = raw;
      changedAt = now;
    }
    if (raw != stable && now - changedAt >= DEBOUNCE_MS) {
      stable = raw;
      if (!stable) {
        pressedAt = now;
        longFired = false;
      } else if (!longFired) {
        shortPress = true;
      }
    }
    if (!stable && !longFired && now - pressedAt >= LONG_PRESS_MS) {
      longFired = true;
      longPress = true;
    }
  }

  bool takeShort() {
    bool v = shortPress;
    shortPress = false;
    return v;
  }
  bool takeLong() {
    bool v = longPress;
    longPress = false;
    return v;
  }
};

static Button keyButton, bootButton;

// Manual override: while set, the mascot stays on whatever you picked.
static bool manualAnim = false;
static int manualIndex = 0;

// ------------------------------------------------------------------ the mood

// The mascot reacts to whichever limit is tightest for the shown brand.
static const char *quotaMood() {
  const BrandState &b = current();
  if (!b.available) return "cloud";
  float worstUsed = 0;
  for (const Limit &l : b.limits) {
    if (l.present && l.shown > worstUsed) worstUsed = l.shown;
  }
  float left = 100.0f - worstUsed;
  if (left >= 75) return "jumping happy";
  if (left >= 50) return "dancing";
  if (left >= 25) return "laptop";
  if (left >= 10) return "pointing";
  return "lurking";
}

// Live session state outranks quota level: what Claude is doing right now is
// more urgent than how much room is left. Falls back to the quota mood when
// hooks aren't installed or the session has gone quiet.
// True while Claude Code is doing something we heard about recently.
static bool claudeActive() {
  return live.valid && millis() - live.receivedAt < 20000 &&
         (live.state == "working" || live.state == "tool" || live.state == "waiting");
}

static const char *moodFor() {
  // The live channel reports Claude Code only, so it must not drive the
  // mascot while the panel is showing ChatGPT's numbers.
  if (brand == BRAND_CLAUDE && live.valid && millis() - live.receivedAt < 20000) {
    if (live.state == "waiting") return "waving";   // Claude needs you
    if (live.state == "tool") {
      if (live.tool == "WebSearch" || live.tool == "WebFetch" || live.tool == "Grep" ||
          live.tool == "Glob")
        return "magnifier";
      if (live.tool == "Bash") return "racing car";
      return "laptop";
    }
    if (live.state == "working") return "laptop";
  }
  return quotaMood();
}

static const splash_anim_def_t *findAnim(const char *name) {
  for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
    if (strcmp(splash_anims[i].name, name) == 0) return &splash_anims[i];
  }
  return &splash_anims[0];
}

static const splash_anim_def_t *currentAnim = nullptr;
static uint16_t animFrame = 0;
static uint32_t animFrameStartedMs = 0;

static void setAnim(const splash_anim_def_t *anim) {
  if (anim == currentAnim) return;
  currentAnim = anim;
  animFrame = 0;
  animFrameStartedMs = millis();
}

static void advanceAnim() {
  if (!currentAnim) return;
  if (millis() - animFrameStartedMs < currentAnim->holds[animFrame]) return;
  animFrameStartedMs = millis();
  animFrame++;
  if (animFrame > currentAnim->loop_end) animFrame = currentAnim->loop_start;
}

// In ChatGPT mode the same art is drawn as an "evil twin": the body takes the
// other brand's colour, the eyes go red, and angled brows are stamped above
// them. Doing it procedurally means all 17 animations get the treatment
// without a second set of artwork.
//
// The eye cells are found rather than hard-coded: in every animation they are
// the darkest palette entry actually referenced by the frame. Unused palette
// slots are also black, so a slot only counts once a cell uses it.
static int eyeIndex(const splash_anim_def_t *a, const uint8_t *cells) {
  int best = -1;
  uint16_t bestLum = 0xFFFF;
  bool used[16] = {false};
  for (int i = 0, n = a->w * a->h; i < n; i++) {
    if (cells[i] && cells[i] < 16) used[cells[i]] = true;
  }
  for (int i = 1; i < a->palette_count && i < 16; i++) {
    if (!used[i]) continue;
    uint16_t c = a->palette[i];
    // Rough luminance on RGB565; good enough to pick out near-black.
    uint16_t lum = ((c >> 11) & 31) * 2 + ((c >> 5) & 63) + (c & 31) * 2;
    if (lum < bestLum) {
      bestLum = lum;
      best = i;
    }
  }
  return bestLum <= 20 ? best : -1;  // only if it really is a dark colour
}

// One angled stroke above an eye cluster, sloping down toward the nose.
static void drawBrow(int x0, int y0, int x1, int y1, int scale, uint16_t colour) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 64; guard++) {
    if (y0 >= 0) stage.fillRect(x0 * scale, y0 * scale, scale, scale, colour);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void drawStage() {
  stage.fillSprite(COL_BG);
  if (currentAnim) {
    const splash_anim_def_t *a = currentAnim;
    // Fit this animation's own bounding box to the area, then centre it. The
    // stage's ox/oy are deliberately ignored: they place the crop on the
    // shared 55x37 grid, which leaves most of the area empty for small art.
    int scale = min(stageAreaW / a->w, stageAreaH / a->h);
    scale = constrain(scale, 1, MAX_CELL);
    int offX = (stageAreaW - a->w * scale) / 2;
    int offY = (stageAreaH - a->h * scale) / 2;
    const bool evil = brand != BRAND_CLAUDE;
    const int eye = evil ? eyeIndex(a, a->frames + (size_t)animFrame * a->w * a->h) : -1;

    const uint8_t *cells = a->frames + (size_t)animFrame * a->w * a->h;
    for (int y = 0; y < a->h; y++) {
      for (int x = 0; x < a->w; x++) {
        uint8_t idx = cells[y * a->w + x];
        if (!idx) continue;  // palette slot 0 is the transparent background
        // The art is authored in Claude coral. In ChatGPT mode retint the body
        // so the mode is unmistakable; darker slots stay put as shading.
        uint16_t colour = a->palette[idx];
        if (evil) {
          if (idx == 1) colour = COL_CHATGPT;
          else if (idx == eye) colour = COL_EVIL_EYE;
        }
        stage.fillRect(offX + x * scale, offY + y * scale, scale, scale, colour);
      }
    }

    if (evil && eye >= 0) {
      // Split the eye cells into left and right clusters, then slope a brow
      // over each: outer edge high, inner edge low.
      int minX = a->w, maxX = -1;
      for (int y = 0; y < a->h; y++) {
        for (int x = 0; x < a->w; x++) {
          if (cells[y * a->w + x] != eye) continue;
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
        }
      }
      if (maxX > minX) {
        int mid = (minX + maxX) / 2;
        for (int side = 0; side < 2; side++) {
          int lo = a->w, hi = -1, top = a->h;
          for (int y = 0; y < a->h; y++) {
            for (int x = 0; x < a->w; x++) {
              if (cells[y * a->w + x] != eye) continue;
              bool left = x <= mid;
              if (left != (side == 0)) continue;
              if (x < lo) lo = x;
              if (x > hi) hi = x;
              if (y < top) top = y;
            }
          }
          if (hi < lo || top < 2) continue;
          int outerY = top - 2, innerY = top - 1;
          int x0 = offX / scale, y0 = offY / scale;  // cell-space origin
          if (side == 0) {
            drawBrow(x0 + lo, y0 + outerY, x0 + hi + 1, y0 + innerY, scale, COL_EVIL_EYE);
          } else {
            drawBrow(x0 + lo - 1, y0 + innerY, x0 + hi, y0 + outerY, scale, COL_EVIL_EYE);
          }
        }
      }
    }
  }
  if (capturing) {
    stage.pushToSprite(&shotBuf, stageX, stageY);
  } else {
    stage.pushSprite(stageX, stageY);
  }
}

// ---------------------------------------------------------------- networking

static void parseLimit(JsonObjectConst node, Limit &l, bool isCredits, const char *bindingKey,
                       const char *myKey) {
  l.present = !node.isNull();
  if (!l.present) return;

  l.percent = node["percent"] | 0.0f;
  l.resetsIn = node["resets_in"].is<long>() ? node["resets_in"].as<long>() : -1;
  l.resetLabel =
      node["resets_label"].is<const char *>() ? node["resets_label"].as<const char *>() : "";
  l.binding = bindingKey && strcmp(bindingKey, myKey) == 0;

  JsonObjectConst trend = node["trend"];
  l.hasTrend = false;
  l.sparkN = 0;
  if (!trend.isNull()) {
    if (trend["per_hour"].is<float>()) {
      l.hasTrend = true;
      l.perHour = trend["per_hour"].as<float>();
      l.exhaustsIn = trend["exhausts_in"].is<long>() ? trend["exhausts_in"].as<long>() : -1;
      l.willExhaust = trend["will_exhaust"] | false;
    }
    for (JsonVariantConst v : trend["spark"].as<JsonArrayConst>()) {
      if (l.sparkN >= (int)(sizeof(l.spark) / sizeof(l.spark[0]))) break;
      l.spark[l.sparkN++] = (uint8_t)constrain((int)(v.as<float>()), 0, 100);
    }
  }

  if (node["value_label"].is<const char *>()) {
    // The server formats money: it knows the currency (AUD here, not USD),
    // and the device has no business guessing at symbols.
    l.value = node["value_label"].as<const char *>();
  } else if (isCredits && node["cap"].is<float>()) {
    l.value = String(node["spent"] | 0.0f, 0) + " of " + String(node["cap"] | 0.0f, 0);
  } else {
    l.value = String((int)roundf(l.percent)) + "%";
  }
}

static void parseBrand(JsonObjectConst node, BrandState &b) {
  b.available = node["available"] | false;
  b.staleFor = node["stale_for"].is<long>() ? node["stale_for"].as<long>() : -1;
  if (!b.available) {
    b.note = node["reason"] | "unavailable";
    for (Limit &l : b.limits) l.present = false;
    return;
  }
  b.note = "";
  const char *binding =
      node["binding"].is<const char *>() ? node["binding"].as<const char *>() : nullptr;
  parseLimit(node["session"], b.limits[0], false, binding, "session");
  parseLimit(node["week"], b.limits[1], false, binding, "week");
  parseLimit(node["credits"], b.limits[2], true, binding, "credits");
}

// Find the server's current address. Falls back to a literal URL if mDNS
// can't answer (some networks block multicast).
static bool resolveUsageUrl() {
  if (!usageUrl.isEmpty()) return true;

  // Browse for the service first. This is what makes moving between machines
  // and networks configuration-free: whichever laptop is running the server
  // advertises _rationai._tcp, and we take the first answer -- no hostname,
  // no IP, nothing stored per-location.
  int found = MDNS.queryService("rationai", "tcp");
  if (found > 0) {
    usageUrl = "http://" + MDNS.IP(0).toString() + ":" + String(MDNS.port(0)) + "/usage";
    statusLine = MDNS.hostname(0);
    return true;
  }

  // A bare IP in the host field skips mDNS entirely.
  IPAddress literal;
  if (literal.fromString(cfgHost)) {
    usageUrl = "http://" + cfgHost + ":" + String(cfgPort) + "/usage";
    return true;
  }

  IPAddress ip = MDNS.queryHost(cfgHost.c_str(), 4000);
  if (ip != IPAddress((uint32_t)0)) {
    usageUrl = "http://" + ip.toString() + ":" + String(cfgPort) + "/usage";
    return true;
  }
  if (strlen(USAGE_URL_FALLBACK) > 0) {
    usageUrl = USAGE_URL_FALLBACK;
    statusLine = "mDNS failed, using fallback";
    return true;
  }
  statusLine = "can't find " + cfgHost;
  return false;
}

static bool fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) {
    statusLine = "wifi lost";
    return false;
  }
  if (!resolveUsageUrl()) return false;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(usageUrl)) {
    statusLine = "bad url";
    usageUrl = "";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    statusLine = "http " + String(code);
    http.end();
    usageUrl = "";  // the server may have moved; look it up again next poll
    return false;
  }

  // Filter to the fields we draw so a growing payload can never outgrow the
  // document.
  JsonDocument filter;
  for (const char *key : BRAND_KEY) {
    filter[key]["available"] = true;
    filter[key]["reason"] = true;
    filter[key]["binding"] = true;
    filter[key]["stale_for"] = true;
    for (const char *w : {"session", "week", "credits"}) {
      filter[key][w]["percent"] = true;
      filter[key][w]["resets_in"] = true;
      filter[key][w]["resets_label"] = true;
      filter[key][w]["spent"] = true;
      filter[key][w]["cap"] = true;
      filter[key][w]["unit"] = true;
      filter[key][w]["value_label"] = true;
      filter[key][w]["trend"]["per_hour"] = true;
      filter[key][w]["trend"]["exhausts_in"] = true;
      filter[key][w]["trend"]["will_exhaust"] = true;
      filter[key][w]["trend"]["spark"] = true;
    }
  }

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    statusLine = String("json: ") + err.c_str();
    return false;
  }

  for (int i = 0; i < BRAND_COUNT; i++) parseBrand(doc[BRAND_KEY[i]], brands[i]);

  fetchedAtMs = millis();
  everFetched = true;
  statusLine = WiFi.localIP().toString();
  return true;
}

// The live channel is a separate, much smaller request than the quota poll.
static void fetchLive() {
  if (WiFi.status() != WL_CONNECTED || usageUrl.isEmpty()) return;

  String url = usageUrl;
  url.replace("/usage", "/live");

  HTTPClient http;
  http.setTimeout(3000);
  http.setConnectTimeout(3000);
  if (!http.begin(url)) return;
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return;

  live.state = doc["state"].is<const char *>() ? doc["state"].as<const char *>() : "unknown";
  live.tool = doc["tool"].is<const char *>() ? doc["tool"].as<const char *>() : "";
  live.detail = doc["detail"].is<const char *>() ? doc["detail"].as<const char *>() : "";
  live.tokensPerSec = doc["tokens_per_sec"] | 0.0f;
  live.sessionCost = doc["session_cost"] | 0.0f;
  live.receivedAt = millis();
  live.valid = live.state != "unknown";
}

// -------------------------------------------------------------------- drawing

static void animateBars() {
  for (BrandState &b : brands) {
    for (Limit &l : b.limits) {
      float target = l.present ? constrain(l.percent, 0.0f, 100.0f) : 0.0f;
      float delta = target - l.shown;
      if (fabsf(delta) < 0.2f) {
        l.shown = target;
      } else {
        l.shown += delta * 0.18f;
      }
    }
  }
}

static void invalidate() {
  for (int i = 0; i < 3; i++) {
    drawnPercent[i] = -999;
    drawnValue[i] = drawnReset[i] = "";
  }
  drawnStatus = drawnAnimLabel = "";
  drawnSparkSig = -1;
  drawnBattery = -999;
  drawnCost = "";
  drawnBrandWord = "";
}

// The bits that never move: header, row labels, bar tracks, divider, brand.
static void drawChrome() {
  g->fillScreen(COL_BG);

  g->setTextDatum(TL_DATUM);
  g->setTextColor(COL_DIM, COL_BG);
  g->drawString("YOUR USAGE LIMITS", PAD, 4, 1);

  for (int i = 0; i < 3; i++) {
    g->setTextDatum(TL_DATUM);
    g->setTextColor(COL_TEXT, COL_BG);
    g->drawString(ROW_LABEL[i], PAD, ROW_TOP[i], 2);
    g->fillSmoothRoundRect(PAD, ROW_TOP[i] + 19, BAR_W, BAR_H, BAR_H / 2, COL_TRACK, COL_BG);
  }

  g->drawFastVLine(DIVIDER_X, 8, 154, COL_TRACK);

  g->setTextDatum(TL_DATUM);
  g->setTextColor(brandColour(), COL_BG);
  g->drawString(BRAND_LABEL[brand], DIVIDER_X + 5, 2, 2);

  chromeDrawn = true;
  invalidate();
}

static void drawRow(int i) {
  Limit &l = current().limits[i];
  const int top = ROW_TOP[i];
  const int barY = top + 19;

  int percent = (int)roundf(l.shown);
  if (percent != drawnPercent[i]) {
    drawnPercent[i] = percent;
    g->fillSmoothRoundRect(PAD, barY, BAR_W, BAR_H, BAR_H / 2, COL_TRACK, COL_BG);
    int filled = (int)roundf(BAR_W * constrain(l.shown, 0.0f, 100.0f) / 100.0f);
    uint16_t colour = barColour(l.shown);
    if (filled >= BAR_H) {
      g->fillSmoothRoundRect(PAD, barY, filled, BAR_H, BAR_H / 2, colour, COL_BG);
    } else if (filled > 0) {
      g->fillRect(PAD, barY, filled, BAR_H, colour);  // too narrow to round
    }
  }

  String value = l.present ? l.value : String("--");
  if (value != drawnValue[i]) {
    drawnValue[i] = value;
    g->fillRect(COL_RIGHT_EDGE - 110, top, 110, 17, COL_BG);
    g->setTextDatum(TR_DATUM);
    // The binding limit is the one to actually watch, so give it the accent.
    g->setTextColor(l.binding ? brandColour() : COL_DIM, COL_BG);
    g->drawString(value, COL_RIGHT_EDGE, top, 2);
  }

  // The 5-hour window ticks down live; the longer ones read better as a
  // wall-clock time, which is how the panel shows them.
  // Not every window reports a reset; say nothing rather than "Resets in --".
  String reset;
  if (l.present) {
    if (!l.resetLabel.isEmpty() && i != 0) {
      reset = "Resets " + l.resetLabel;
    } else if (liveResetsIn(l) >= 0) {
      reset = "Resets in " + humanDuration(liveResetsIn(l));
    }
  }
  if (reset != drawnReset[i]) {
    drawnReset[i] = reset;
    g->fillRect(PAD, barY + BAR_H + 2, BAR_W, 10, COL_BG);
    g->setTextDatum(TR_DATUM);
    g->setTextColor(COL_DIM, COL_BG);
    g->drawString(reset, COL_RIGHT_EDGE, barY + BAR_H + 2, 1);
  }
}

static const Limit &trendLimit() {
  BrandState &b = current();
  for (const Limit &l : b.limits) {
    if (l.present && l.binding) return l;
  }
  return b.limits[0];
}

// Fixed 0-100 scale rather than autoscaling: an autoscaled trace makes a flat
// 20-22% hour look like a cliff.
static void drawSpark() {
  const Limit &l = trendLimit();
  int sig = brand * 100000 + l.sparkN * 1000 + (l.sparkN ? l.spark[l.sparkN - 1] : 0) +
            (int)(l.perHour * 10);
  if (sig == drawnSparkSig) return;
  drawnSparkSig = sig;

  g->fillRect(SPARK_X, SPARK_Y, SPARK_W, SPARK_H + 14, COL_BG);
  g->drawFastHLine(SPARK_X, SPARK_Y + SPARK_H, SPARK_W, COL_TRACK);

  if (l.sparkN >= 2) {
    uint16_t colour = barColour(l.shown);
    int prevX = 0, prevY = 0;
    for (int i = 0; i < l.sparkN; i++) {
      int x = SPARK_X + (SPARK_W - 1) * i / (l.sparkN - 1);
      int y = SPARK_Y + SPARK_H - 1 - (SPARK_H - 2) * l.spark[i] / 100;
      if (i) g->drawLine(prevX, prevY, x, y, colour);
      prevX = x;
      prevY = y;
    }
  } else {
    g->setTextDatum(MC_DATUM);
    g->setTextColor(COL_TRACK, COL_BG);
    g->drawString("collecting trend", SPARK_X + SPARK_W / 2, SPARK_Y + SPARK_H / 2, 1);
  }

  String caption;
  if (l.hasTrend) caption = (l.perHour >= 0 ? "+" : "") + String(l.perHour, 1) + "%/h";
  g->setTextDatum(TC_DATUM);
  g->setTextColor(COL_DIM, COL_BG);
  g->drawString(caption, SPARK_X + SPARK_W / 2, SPARK_Y + SPARK_H + 3, 1);
}

static void drawAnimLabel() {
  // What Claude is doing outranks the animation's own name here -- "needs you"
  // is the whole point of the live channel.
  String label;
  uint16_t colour = COL_TRACK;
  bool fresh = brand == BRAND_CLAUDE && live.valid && millis() - live.receivedAt < 20000;

  if (manualAnim) {
    label = String(currentAnim ? currentAnim->name : "") + "   -   " + String(manualIndex + 1) +
            "/" + String(SPLASH_ANIM_COUNT);
    colour = brandColour();
  } else if (fresh && live.state == "waiting") {
    label = live.detail.length() ? live.detail : "needs you";
    colour = COL_WARN;
  } else if (fresh && live.state == "tool") {
    label = live.tool.length() ? live.tool : "working";
    colour = COL_DIM;
  } else if (fresh && live.state == "working") {
    label = "thinking";
    colour = COL_DIM;
  } else {
    label = currentAnim ? currentAnim->name : "";
  }

  if (label == drawnAnimLabel) return;
  drawnAnimLabel = label;
  g->fillRect(DIVIDER_X + 2, 96, 320 - DIVIDER_X - 2, 10, COL_BG);
  g->setTextDatum(TC_DATUM);
  g->setTextColor(colour, COL_BG);
  g->drawString(label, CX_RIGHT, 96, 1);
}

// A live burn meter under the sparkline: tokens/sec on a log scale, because
// bursts span three orders of magnitude and a linear bar is pinned or empty.
// Session cost. This owns the y142-158 band on its own; the trend caption
// above carries the rate, so nothing else draws here.
static void drawCost() {
  bool fresh = brand == BRAND_CLAUDE && live.valid && millis() - live.receivedAt < 20000;
  String cost = fresh || live.sessionCost > 0 ? "$" + String(live.sessionCost, 2) : "";
  if (cost == drawnCost) return;
  drawnCost = cost;
  g->fillRect(DIVIDER_X + 2, 142, 320 - DIVIDER_X - 2, 16, COL_BG);
  g->setTextDatum(TC_DATUM);
  g->setTextColor(COL_DIM, COL_BG);
  g->drawString(cost, CX_RIGHT, 142, 2);
}

static void drawBattery() {
  float volts = batteryVolts();
  // Plugged in, the charger drives this rail to roughly USB voltage -- well
  // above a 3.7V cell's 4.2V full mark. That reading means external power is
  // present, not that the sense pin is faulty.
  bool usb = volts >= 4.30f;
  bool present = volts >= 2.5f;  // below this nothing is on the sense pin
  bool charging = present && (usb || batteryCharging(volts));
  // While the charger is driving the rail the cell's true state of charge
  // isn't measurable, so show it full rather than guess.
  int pct = !present ? -1 : (usb ? 100 : batteryPercent(volts));

  int sig = pct * 2 + (charging ? 1 : 0);
  if (sig == drawnBattery) return;
  drawnBattery = sig;

  const int x = portrait ? P_W - 34 : 282;
  const int y = 2;
  g->fillRect(x - 2, y, 34, 13, COL_BG);
  if (pct < 0) return;
  drawBatteryIcon(x, y, pct, charging);
}

static void drawStatus() {
  BrandState &b = current();
  String text = b.available ? statusLine : b.note;
  uint16_t colour = COL_TRACK;
  if (!b.available) {
    colour = COL_DANGER;
  } else if (b.staleFor > 120) {
    text = "stale " + humanDuration(b.staleFor) + " -- numbers may have moved";
    colour = COL_WARN;
  }
  if (text == drawnStatus) return;
  drawnStatus = text;
  g->fillRect(PAD, 158, BAR_W, 10, COL_BG);
  g->setTextDatum(TL_DATUM);
  g->setTextColor(colour, COL_BG);
  g->drawString(text, PAD, 158, 1);
}

// Show whichever brand is nearest a cap. Only switches on a clear margin so
// two brands sitting a point apart don't make the panel flip back and forth.
// A crossing is worth interrupting for; the level itself already shows in the
// bar colour, so the flash fires once rather than nagging.
static void flashAlert(uint16_t colour) {
  for (int i = 0; i < 2; i++) {
    g->fillScreen(colour);
    delay(60);
    g->fillScreen(COL_BG);
    delay(60);
  }
  chromeDrawn = false;
}

static void checkAlerts() {
  static float lastWorst[BRAND_COUNT] = {-1, -1};
  for (int i = 0; i < BRAND_COUNT; i++) {
    float now = worstUsed(brands[i]);
    float before = lastWorst[i];
    if (now < 0) continue;
    if (before >= 0) {
      for (float level : ALERT_LEVELS) {
        if (before < level && now >= level) {
          // Flash, but leave the brand alone: the displayed mode is the
          // user's choice and nothing should change it but the user.
          flashAlert(level >= 90 ? COL_DANGER : COL_WARN);
          break;
        }
      }
      // A window rolling over is the good news, and worth a beat too.
      if (before > 40 && now < 5) {
        flashAlert(i == BRAND_CLAUDE ? COL_CLAUDE : COL_CHATGPT);
      }
    }
    lastWorst[i] = now;
  }
}

// ------------------------------------------------------------ portrait totem
//
// A different arrangement, not a rotated one: Clawd at 3x pixel scale as the
// centrepiece, the wordmark under him, then the three limits as a stack, and
// the trend as a filled area rather than a line.

static void drawChromePortrait() {
  g->fillScreen(COL_BG);

  g->setTextDatum(TL_DATUM);
  g->setTextColor(COL_TRACK, COL_BG);
  g->drawString("RATION AI", P_PAD, 3, 1);

  for (int i = 0; i < 3; i++) {
    g->setTextDatum(TL_DATUM);
    g->setTextColor(COL_TEXT, COL_BG);
    g->drawString(P_ROW_LABEL[i], P_PAD, P_ROW_TOP[i], 2);
    g->fillSmoothRoundRect(P_PAD, P_ROW_TOP[i] + 18, P_BAR_W, BAR_H, BAR_H / 2, COL_TRACK,
                            COL_BG);
  }

  chromeDrawn = true;
  invalidate();
}

static void drawBrandWordPortrait() {
  String word = BRAND_LABEL[brand];
  if (word == drawnBrandWord) return;
  drawnBrandWord = word;
  g->fillRect(0, P_BRAND_Y, P_W, 44, COL_BG);
  g->setTextDatum(TC_DATUM);
  g->setTextColor(brandColour(), COL_BG);
  // Supercharge Condensed 18pt: CHATGPT measures 158x40px. It fits the 166px
  // width at full size, so the layout gives it a 44px slot rather than
  // shrinking a display face into illegibility.
  g->setFreeFont(&SuperchargeCn18);
  g->drawString(BRAND_LABEL[brand], P_CX, P_BRAND_Y);
  g->setTextFont(1);  // hand the built-in fonts back to everything else
}

// "A$41 of A$70" won't fit beside a font-2 label in 154px, so tighten it.
static String compactValue(const String &v) {
  String out = v;
  out.replace(" of ", "/");
  return out;
}

static void drawRowPortrait(int i) {
  Limit &l = current().limits[i];
  const int top = P_ROW_TOP[i];
  const int barY = top + 18;

  int percent = (int)roundf(l.shown);
  if (percent != drawnPercent[i]) {
    drawnPercent[i] = percent;
    g->fillSmoothRoundRect(P_PAD, barY, P_BAR_W, BAR_H, BAR_H / 2, COL_TRACK, COL_BG);
    int filled = (int)roundf(P_BAR_W * constrain(l.shown, 0.0f, 100.0f) / 100.0f);
    uint16_t colour = barColour(l.shown);
    if (filled >= BAR_H) {
      g->fillSmoothRoundRect(P_PAD, barY, filled, BAR_H, BAR_H / 2, colour, COL_BG);
    } else if (filled > 0) {
      g->fillRect(P_PAD, barY, filled, BAR_H, colour);
    }
  }

  String value = l.present ? compactValue(l.value) : String("--");
  if (value != drawnValue[i]) {
    drawnValue[i] = value;
    g->fillRect(P_CX - 20, top - 1, P_W - P_CX + 20 - P_PAD, 18, COL_BG);
    g->setTextDatum(TR_DATUM);
    g->setTextColor(l.binding ? brandColour() : COL_DIM, COL_BG);
    g->drawString(value, P_W - P_PAD, top, 2);
  }
}

static void drawLivePortrait() {
  String label;
  uint16_t colour = COL_TRACK;
  BrandState &b = current();

  // Data health outranks everything else here. Portrait has no status line, so
  // this is the only place a frozen or missing reading can announce itself --
  // and a number with no warning attached reads as current.
  if (!b.available) {
    label = b.note.length() > 22 ? b.note.substring(0, 21) + "\xC9" : b.note;
    if (label.isEmpty()) label = "no data";
    colour = COL_DANGER;
    if (label == drawnAnimLabel) return;
    drawnAnimLabel = label;
    g->fillRect(0, P_LIVE_Y, P_W, 16, COL_BG);
    g->setTextDatum(TC_DATUM);
    g->setTextColor(colour, COL_BG);
    g->drawString(label, P_CX, P_LIVE_Y, 2);
    return;
  }
  if (b.staleFor > 120) {
    label = "stale " + humanDuration(b.staleFor);
    colour = COL_WARN;
    if (label == drawnAnimLabel) return;
    drawnAnimLabel = label;
    g->fillRect(0, P_LIVE_Y, P_W, 16, COL_BG);
    g->setTextDatum(TC_DATUM);
    g->setTextColor(colour, COL_BG);
    g->drawString(label, P_CX, P_LIVE_Y, 2);
    return;
  }

  // Same gate as landscape: the live channel is Claude Code only.
  bool fresh = brand == BRAND_CLAUDE && live.valid && millis() - live.receivedAt < 20000;
  if (manualAnim) {
    label = currentAnim ? currentAnim->name : "";
    colour = brandColour();
  } else if (fresh && live.state == "waiting") {
    label = live.detail.length() ? live.detail : "needs you";
    colour = COL_WARN;
  } else if (fresh && live.state == "tool") {
    label = live.tool.length() ? live.tool : "working";
    colour = COL_DIM;
  } else if (fresh && live.state == "working") {
    label = "thinking";
    colour = COL_DIM;
  } else if (live.sessionCost > 0) {
    label = "$" + String(live.sessionCost, 2);
    colour = COL_DIM;
  }

  if (label == drawnAnimLabel) return;
  drawnAnimLabel = label;
  g->fillRect(0, P_LIVE_Y, P_W, 16, COL_BG);
  g->setTextDatum(TC_DATUM);
  g->setTextColor(colour, COL_BG);
  g->drawString(label, P_CX, P_LIVE_Y, 2);
}

// Swap layouts: the panel rotation, the geometry, and the sprite all change.
static void setPortrait(bool want) {
  if (want == portrait) return;
  portrait = want;

  tft.setRotation(portrait ? 0 : 1);
  stageX = portrait ? P_STAGE_X : STAGE_X;
  stageY = portrait ? P_STAGE_Y : STAGE_Y;
  stageAreaW = portrait ? P_STAGE_W : STAGE_PX_W;
  stageAreaH = portrait ? P_STAGE_H : STAGE_PX_H;

  stage.deleteSprite();
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // full range for a 4.2V cell after the 2:1 divider

  stage.setColorDepth(16);
  stage.createSprite(stageAreaW, stageAreaH);

  drawnBrandWord = "";
  chromeDrawn = false;  // full repaint in the new arrangement
}

static void splash(const String &message) {
  g->fillScreen(COL_BG);
  g->setTextDatum(MC_DATUM);
  g->setTextColor(COL_CLAUDE, COL_BG);
  g->setFreeFont(&SuperchargeCn18);
  g->drawString("RationAI", 160, 70);
  g->setTextFont(1);
  g->setTextColor(COL_DIM, COL_BG);
  g->drawString(message, 160, 100, 2);
  chromeDrawn = false;
}

// Full brightness only when something actually wants attention.
static void updateBacklight() {
  BrandState &b = current();
  float worstUsed = 0;
  for (const Limit &l : b.limits) {
    if (l.present && l.shown > worstUsed) worstUsed = l.shown;
  }
  bool calm = b.available && worstUsed <= 75;
  uint8_t want = calm ? BL_DIM : BL_BRIGHT;
  if (want != backlight) {
    backlight = want;
    analogWrite(TFT_BL, backlight);
  }
}

// Paint every element. Cheap on a normal frame because each piece repaints
// only when its own value changed; a forced full repaint happens when chrome
// is invalidated.
static void renderAll() {
  if (!chromeDrawn) portrait ? drawChromePortrait() : drawChrome();
  drawStage();
  if (portrait) {
    drawBrandWordPortrait();
    for (int i = 0; i < 3; i++) drawRowPortrait(i);
    drawLivePortrait();
  } else {
    for (int i = 0; i < 3; i++) drawRow(i);
    drawAnimLabel();
    drawSpark();
    drawCost();
    drawStatus();
  }
  drawBattery();
}

// ------------------------------------------------------------- screenshots
//
// Reads the panel back over the parallel bus and streams it as base64 rows, so
// documentation shows the real device output rather than a mock-up. Triggered
// by sending 's' (current screen) or 'g' (step the animation, then dump) on
// the serial port; costs nothing when unused.

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void dumpBuffer(uint16_t *px, int w, int h) {
  Serial.printf("SHOT %d %d\n", w, h);
  for (int y = 0; y < h; y++) {
    uint32_t acc = 0;
    int bits = 0;
    for (int x = 0; x < w; x++) {
      uint16_t v = px[y * w + x];
      for (int b = 0; b < 2; b++) {
        acc = (acc << 8) | (b ? (v & 0xFF) : (v >> 8));
        bits += 8;
        while (bits >= 6) {
          bits -= 6;
          Serial.write(B64[(acc >> bits) & 0x3F]);
        }
      }
    }
    if (bits) Serial.write(B64[(acc << (6 - bits)) & 0x3F]);
    Serial.println();
  }
  Serial.println("ENDSHOT");
}

// Re-render the whole UI into an off-screen buffer and stream that. Reading
// the panel back gives 18-bit colour that does not survive the trip to 16-bit,
// so the only trustworthy pixels are the ones we draw ourselves.
static void dumpScreen() {
  const int w = tft.width(), h = tft.height();
  if (!shotBuf.createSprite(w, h)) {
    Serial.println("SHOTFAIL no memory");
    return;
  }

  capturing = true;
  g = &shotBuf;
  chromeDrawn = false;  // force a complete repaint into the buffer
  renderAll();
  g = &tft;
  capturing = false;

  dumpBuffer((uint16_t *)shotBuf.getPointer(), w, h);
  shotBuf.deleteSprite();

  chromeDrawn = false;  // and repaint the real panel
}

static void handleSerialCommands() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c == 's') {
    dumpScreen();
  } else if (c == 'g') {
    // Step to the next animation and settle a frame before dumping.
    manualAnim = true;
    manualIndex = (manualIndex + 1) % SPLASH_ANIM_COUNT;
    setAnim(&splash_anims[manualIndex]);
    drawStage();
    delay(40);
    dumpScreen();
  } else if (c == 'f') {
    // Advance one animation frame in place, for capturing a loop.
    animFrame = (animFrame + 1 > currentAnim->loop_end) ? currentAnim->loop_start : animFrame + 1;
    drawStage();
    delay(20);
    dumpScreen();
  } else if (c == 'v') {
    float v = batteryVolts();
    Serial.printf("BATT raw_mV=%u volts=%.3f pct=%d charging=%d\n",
                  analogReadMilliVolts(PIN_BATTERY), v, batteryPercent(v),
                  batteryCharging(v) ? 1 : 0);
  } else if (c == 'b') {
    brand = (brand + 1) % BRAND_COUNT;
    prefs.putInt("brand", brand);
    chromeDrawn = false;
  } else if (c == 'r') {
    setPortrait(!portrait);
  }
}

// ----------------------------------------------------------------- lifecycle

// Restyles the setup portal to match the display: WiFiManager appends this
// after its own stylesheet, so these rules win and the markup it emits is
// left alone.
static const char PORTAL_CSS[] PROGMEM = R"CSS(
<style>
:root{color-scheme:dark;--bg:#141413;--card:#1d1d1b;--line:#2e2c29;--tx:#f0eee6;--dim:#8a8780;--ac:#d97757}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--tx);margin:0;padding:26px 16px 44px;
-webkit-font-smoothing:antialiased;text-align:left;
font:15px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}
.wrap{max-width:420px;min-width:0;width:100%;margin:0 auto;display:block}
h1{display:none}
.wrap::before{content:'RationAI';display:block;font-size:26px;font-weight:650;
letter-spacing:-.4px;padding-bottom:15px;margin-bottom:20px;
border-bottom:1px solid var(--line)}
h3{margin:-8px 0 22px;font-size:13px;font-weight:500;color:var(--dim)}
/* the library separates blocks with stray <br>; the margins do that here */
.wrap>br,form br{display:none}
hr{border:0;border-top:1px solid var(--line);margin:24px 0}
/* network rows: the library emits one div per scan result */
.wrap>div{display:flex;align-items:center;gap:10px;background:var(--card);
border:1px solid var(--line);border-radius:12px;padding:13px 14px;margin:8px 0}
a{flex:1;min-width:0;display:block;color:var(--tx);font-weight:600;
text-decoration:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
a:hover{color:var(--ac);text-decoration:none}
.q{filter:invert(1);opacity:.75;float:none;flex:none;padding:0;margin:0}
label{display:block;margin:18px 0 7px;font-size:11px;font-weight:700;
letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}
input{width:100%;margin:0;padding:13px 14px;font-size:16px;color:var(--tx);
background:var(--card);border:1px solid var(--line);border-radius:12px}
input:focus{outline:0;border-color:var(--ac);box-shadow:0 0 0 3px rgba(217,119,87,.2)}
input::placeholder{color:#5f5c56}
#showpass{width:17px;height:17px;margin:15px 0 0;vertical-align:middle;accent-color:var(--ac)}
label[for=showpass]{display:inline;margin-left:7px;font-size:14px;font-weight:500;
letter-spacing:0;text-transform:none;color:var(--dim);vertical-align:middle}
button{width:100%;margin:9px 0;padding:14px 16px;font-size:15px;font-weight:650;
line-height:1.2;color:var(--tx);background:var(--card);border:1px solid var(--line);
border-radius:12px;cursor:pointer;transition:none}
button:hover{border-color:#403d38}
button:active{opacity:1 !important;transform:translateY(1px)}
button[type=submit],form[action='/wifi'] button{background:var(--ac);
border-color:var(--ac);color:#1b100b}
button.D{background:transparent;border-color:#8c3a22;color:#e08363}
.msg{margin:24px 0 0;padding:16px;color:var(--dim);background:var(--card);
border:1px solid var(--line);border-left:3px solid var(--dim);border-radius:12px}
.msg strong{color:var(--tx)}
.msg.S{border-left-color:#10a37f}
.msg.D{border-left-color:#bf4722}
dt{color:var(--dim);font-weight:600}
dd{color:var(--tx);padding-bottom:12px}
td,th{padding:4px 8px 4px 0;text-align:left}
</style>
)CSS";

void setup() {
  Serial.begin(115200);

  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  keyButton.begin(PIN_KEY);
  bootButton.begin(PIN_BOOT);

  tft.init();
  tft.setRotation(1);  // landscape, 320x170
  tft.setSwapBytes(true);

  COL_BG = tft.color565(20, 20, 19);
  COL_TEXT = tft.color565(240, 238, 230);
  COL_DIM = tft.color565(138, 135, 128);
  COL_TRACK = tft.color565(46, 44, 41);
  COL_CLAUDE = tft.color565(217, 119, 87);   // Claude coral
  COL_CHATGPT = tft.color565(16, 163, 127);  // OpenAI green
  COL_WARN = tft.color565(217, 167, 87);
  COL_DANGER = tft.color565(191, 71, 34);
  COL_EVIL_EYE = tft.color565(232, 40, 40);  // the evil twin's eyes and brows

  stage.setColorDepth(16);
  stage.createSprite(stageAreaW, stageAreaH);

  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, backlight);

  setAnim(findAnim("walking"));

  // Namespace kept from the project's original name so an existing device
  // doesn't lose its stored networks on upgrade.
  prefs.begin("howmuchai", false);
  cfgHost = prefs.getString("host", USAGE_HOST);
  cfgPort = prefs.getInt("port", USAGE_PORT);
  brand = constrain(prefs.getInt("brand", BRAND_CLAUDE), 0, BRAND_COUNT - 1);

  splash("connecting to wifi");

  WiFiManager wm;
  static const char *PORTAL_MENU[] = {"wifi", "info", "sep", "exit"};
  wm.setTitle("RationAI");
  wm.setCustomHeadElement(PORTAL_CSS);
  wm.setMenu(PORTAL_MENU, 4);
  char hostBuf[64], portBuf[8];
  strlcpy(hostBuf, cfgHost.c_str(), sizeof(hostBuf));
  snprintf(portBuf, sizeof(portBuf), "%d", cfgPort);
  WiFiManagerParameter hostParam("host", "Server host (or IP)", hostBuf, sizeof(hostBuf));
  WiFiManagerParameter portParam("port", "Server port", portBuf, sizeof(portBuf));
  wm.addParameter(&hostParam);
  wm.addParameter(&portParam);

  // Tell the user what to join -- the portal is useless if it's invisible.
  wm.setAPCallback([](WiFiManager *) {
    splash("join wifi 'RationAI-Setup'");
  });
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(20);

  // Reuses stored credentials when it has them; only opens the portal when it
  // can't connect. Holding KEY at boot forces the portal for a deliberate
  // reconfigure.
  bool forcePortal = digitalRead(PIN_KEY) == LOW;

  // Try every network we've been told about before opening the portal, so
  // moving between home, work and a phone hotspot needs no setup at all.
  bool connected = false;
  if (!forcePortal) {
    splash("looking for known wifi");
    connected = connectRemembered(15000);
  }
  if (!connected) {
    connected = forcePortal ? wm.startConfigPortal("RationAI-Setup")
                            : wm.autoConnect("RationAI-Setup");
    // Whatever the portal just joined, add it to the list rather than
    // replacing it -- that's the whole point of keeping four.
    if (connected) rememberNetwork(wm.getWiFiSSID(), wm.getWiFiPass());
  }

  String newHost = hostParam.getValue();
  int newPort = atoi(portParam.getValue());
  if (newHost.length() && (newHost != cfgHost || newPort != cfgPort)) {
    cfgHost = newHost;
    if (newPort > 0) cfgPort = newPort;
    prefs.putString("host", cfgHost);
    prefs.putInt("port", cfgPort);
    usageUrl = "";
  }

  if (!connected) {
    splash("no wifi -- hold KEY at boot to set up");
    delay(2500);
  } else {
    MDNS.begin("rationai");
    splash("finding the server");
  }

  if (prefs.getBool("portrait", false)) setPortrait(true);

  fetchUsage();
  lastPollMs = millis();
}

void loop() {
  bool due = millis() - lastPollMs >= POLL_INTERVAL_MS;

  handleSerialCommands();
  keyButton.update();
  bootButton.update();

  // Presses are counted and resolved a short time after the last one rather
  // than acted on instantly, so a double tap is never mistaken for two single
  // taps. 400ms is imperceptible in use and comfortably separates the two.
  static uint32_t bootLastPressMs = 0;
  static int bootPressCount = 0;

  static uint32_t keyLastPressMs = 0;
  static int keyPressCount = 0;

  if (keyButton.takeShort()) {
    keyPressCount++;
    keyLastPressMs = millis();
  }

  if (keyPressCount && millis() - keyLastPressMs >= MULTI_PRESS_MS) {
    int presses = keyPressCount;
    keyPressCount = 0;

    if (presses >= 2) {
      setPortrait(!portrait);
      prefs.putBool("portrait", portrait);  // remember the layout too
    } else {
      if (!manualAnim) {
        manualAnim = true;
        manualIndex = currentAnim ? (int)(currentAnim - splash_anims) : 0;
      }
      manualIndex = (manualIndex + presses) % SPLASH_ANIM_COUNT;
      setAnim(&splash_anims[manualIndex]);
    }
  }

  if (bootButton.takeShort()) {
    bootPressCount++;
    bootLastPressMs = millis();
  }

  if (bootPressCount && millis() - bootLastPressMs >= MULTI_PRESS_MS) {
    int presses = bootPressCount;
    bootPressCount = 0;

    if (presses >= 2) {
      brand = (brand + 1) % BRAND_COUNT;
      prefs.putInt("brand", brand);  // survives a reboot
      chromeDrawn = false;           // repaint in the new brand's colours
    } else {
      if (!manualAnim) {
        manualAnim = true;
        manualIndex = currentAnim ? (int)(currentAnim - splash_anims) : 0;
      }
      manualIndex = (manualIndex - presses + SPLASH_ANIM_COUNT * presses) % SPLASH_ANIM_COUNT;
      setAnim(&splash_anims[manualIndex]);
    }
  }

  // Each long press releases its own override and nothing else, so freeing
  // the animation never disturbs the displayed brand.
  // Holding either button drops any pinned animation and refreshes, so the
  // display goes back to showing what is happening right now.
  if (keyButton.takeLong() || bootButton.takeLong()) {
    manualAnim = false;
    keyPressCount = 0;
    bootPressCount = 0;
    due = true;
  }

  if (due) {
    if (WiFi.status() != WL_CONNECTED) {
      // Moving networks mid-session: run the list again rather than retrying
      // one AP that may no longer be in range.
      if (wifiMulti.run(8000) != WL_CONNECTED) WiFi.reconnect();
      usageUrl = "";  // force rediscovery on the new network
    }
    if (!fetchUsage() && !everFetched) {
      for (BrandState &b : brands) b.note = statusLine;
    }
    lastPollMs = millis();
    checkAlerts();
  }

  static uint32_t lastLiveMs = 0;
  if (millis() - lastLiveMs >= LIVE_INTERVAL_MS) {
    lastLiveMs = millis();
    fetchLive();
  }

  static uint32_t lastFrameMs = 0;
  if (millis() - lastFrameMs >= FRAME_MS) {
    lastFrameMs = millis();

    animateBars();
    if (!manualAnim) setAnim(findAnim(moodFor()));
    advanceAnim();
    updateBacklight();
    renderAll();
  }

  delay(2);
}
