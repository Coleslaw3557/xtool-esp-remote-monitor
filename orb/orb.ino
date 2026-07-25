// xTool Buddy Orb — a strictly READ-ONLY remote status display for the xTool
// S1 laser, on the Guition JC3636W518C round 360x360 panel (see board.h).
//
// The orb auto-discovers the S1 on the LAN (xtool_s1.h) and mirrors its state
// as a cute illustrated buddy (art_states.h) with an ember progress arc, big
// time digits, and a status label (ui_buddy.h):
//   no wifi / laser missing / link lost -> searching buddy (flashlight)
//   idle -> tea break     sleeping -> zzz       job ready -> green flag
//   framing -> dashed box cutting -> goggles+sparks (countdown if estimated)
//   paused -> pause bars  finished -> confetti  errors -> warning triangle
//
// READ-ONLY GUARANTEE: the only bytes ever sent to the laser are the
// documented read-only queries in xtool_s1.h (M222/M2003 + ws pong). The UI
// has no control paths — this display can never start, stop, or alter a job.
//
// Touch: a tap toggles the backlight between bright and a night-friendly dim.
// OTA: hostname xtool-orb / port 3232 (./build.sh ota).

#include <Arduino_GFX_Library.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include <math.h>

#include "art_states.h"
#include "board.h"
#include "secrets.h"
#include "ui_buddy.h"
// where discovery last found the S1; rediscovery still tracks DHCP moves
#define XTOOL_FALLBACK_IP "192.168.253.167"
#include "xtool_s1.h"

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_CLK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
Arduino_GFX *panel = new Arduino_ST77916(bus, LCD_RST, 0, true, LCD_W, LCD_H);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_W, LCD_H, panel);

uint16_t *artLayer = nullptr; // current screen's expanded art, PSRAM
bool havePanel = false, haveTouch = false;
bool backlightOn = false, backlightDim = false;

// everything the renderer needs for one frame of UI
struct UiModel {
  ui::Screen scr;
  const char *lbl;
  bool digits;
  uint32_t secs;
  float frac; // progress arc fraction, -1 = no arc
  bool dim;   // dim palette + pulsing label (paused / link trouble)
};

// ---------- CST816 tap-to-dim ----------
struct TouchState {
  bool down = false;
  int x = 0, y = 0;
};
TouchState touch;
bool wasDown = false;
uint32_t downAt = 0;

static void touchReset() {
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(8);
  digitalWrite(TOUCH_RST, HIGH);
  delay(120); // chip ACKs in the window right after reset
}

static bool cstWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int cstRead(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  size_t got = Wire.requestFrom((int)CST816_ADDR, (int)n);
  for (size_t i = 0; i < got; i++) buf[i] = Wire.read();
  return (int)got;
}

static bool touchPoll(TouchState &t) {
  uint8_t d[6];
  if (cstRead(0x01, d, 6) != 6) { // asleep chips NACK until first touch
    t.down = false;
    return false;
  }
  t.down = d[1] > 0;
  if (t.down) {
    t.x = ((d[2] & 0x0F) << 8) | d[3];
    t.y = ((d[4] & 0x0F) << 8) | d[5];
  }
  return true;
}

static void setBacklight(bool on, bool dim) {
  backlightOn = on;
  backlightDim = dim;
  ledcWrite(BL_GPIO, !on ? 0 : (dim ? 40 : 255));
}

// ---------- dirty-rect blit ----------
static uint16_t blitScratch[360 * 56];

static void blitRect(int x0, int y0, int x1, int y1) {
  int w = x1 - x0;
  if (w <= 0 || y1 <= y0) return;
  int maxRows = (int)(sizeof(blitScratch) / sizeof(blitScratch[0]) / w);
  if (maxRows < 1) return;
  uint16_t *fb = gfx->getFramebuffer();
  for (int ys = y0; ys < y1; ys += maxRows) {
    int h = ys + maxRows < y1 ? maxRows : y1 - ys;
    for (int y = 0; y < h; y++)
      memcpy(blitScratch + y * w, fb + (ys + y) * LCD_W + x0, (size_t)w * 2);
    panel->draw16bitRGBBitmap(x0, ys, blitScratch, w, h);
  }
}

// ---------- screen selection ----------
static UiModel pickModel(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return {ui::SCR_SEARCH, "WIFI", false, 0, -1, true};
  bool link = xtool::linkUp();
  xtool::Phase p = xtool::phase();
  uint32_t el = xtool::elapsedMs(now) / 1000;
  int tot = xtool::totalSec(now);
  uint32_t remain = tot > 0 ? (el < (uint32_t)tot ? tot - el : 0) : el;

  if (!link) {
    if (p != xtool::PH_NONE && p != xtool::PH_DONE) // mid-job blackout: keep the clock up
      return {ui::SCR_SEARCH, "NO LINK", true, remain, -1, true};
    return {ui::SCR_SEARCH, "LOOKING", false, 0, -1, true};
  }
  if (p == xtool::PH_DONE) return {ui::SCR_DONE, "DONE", true, el, 1.0f, false};

  switch (xtool::rawStatus()) {
    case 14: {
      float fr = tot > 0 ? ui::clampf((float)el / tot, 0, 1) : (el % 60) / 60.0f;
      return {ui::SCR_CUT, tot > 0 ? "REMAINING" : "CUTTING", true, remain, fr, false};
    }
    case 15: {
      float fr = tot > 0 ? ui::clampf((float)el / tot, 0, 1) : (el % 60) / 60.0f;
      return {ui::SCR_PAUSE, "PAUSED", true, remain, fr, true};
    }
    case 12: return {ui::SCR_FRAME, "FRAMING", true, el, -1, false};
    case 10:
    case 24: return {ui::SCR_READY, "MEASURING", tot > 0, (uint32_t)tot, -1, false};
    case 11:
    case 13: return {ui::SCR_READY, "READY", tot > 0, (uint32_t)tot, -1, false};
    case 17: return {ui::SCR_SLEEP, "ASLEEP", false, 0, -1, true};
    case 16: return {ui::SCR_IDLE, "UPDATING", false, 0, -1, true};
    case 18: return {ui::SCR_IDLE, "STOPPING", false, 0, -1, false};
    case 2: return {ui::SCR_ERROR, "SETUP MODE", false, 0, -1, true};
    // error family, meanings per xTool's support articles (see xtool_s1.h)
    case 9: return {ui::SCR_ERR_FLAME, "FLAME ALARM", false, 0, -1, false};
    case 4: return {ui::SCR_ERR_MOVED, "MOVED", false, 0, -1, false};
    case 7: return {ui::SCR_ERR_LID, "LID OPEN", false, 0, -1, false};
    case 20: return {ui::SCR_ERR_LIMIT, "HIT LIMIT", false, 0, -1, false};
    case 21: return {ui::SCR_ERR_WIFI, "WIFI FAULT", false, 0, -1, false};
    case 22: return {ui::SCR_ERR_LASER, "LASER FAULT", false, 0, -1, false};
    default: return {ui::SCR_IDLE, "IDLE", false, 0, -1, false};
  }
}

// ---------- render bookkeeping ----------
ui::Screen curScr = ui::SCR_COUNT; // forces the first showScreen
bool curDigits = false;
char curLbl[14] = "";
uint32_t curSecs = 0xFFFFFFFF;
float curFrac = -2, curPulseQ = -1;
bool curDim = false;
uint32_t lastSpark = 0;
uint32_t lastFrame = 0, bootMs = 0, lastBeat = 0;

static void paintDelta(const UiModel &m, uint32_t now, bool full) {
  uint16_t *fb = gfx->getFramebuffer();
  float amt = m.dim ? 0.45f : 1.0f;
  int bx0, by0, bx1, by1;

  if (m.frac >= 0) {
    float spark = m.scr == ui::SCR_CUT ? 0.5f + 0.5f * sinf(now * 0.021f) : 0.0f;
    bool wrap = m.frac < curFrac - 0.001f;
    if (full || wrap || curFrac < 0 || m.dim != curDim) {
      ui::arcPaint(fb, artLayer, 0, m.frac, amt, spark, bx0, by0, bx1, by1);
      if (!full) blitRect(bx0, by0, bx1, by1);
      curFrac = m.frac;
    } else if (m.frac - curFrac > 0.004f ||
               (m.scr == ui::SCR_CUT && now - lastSpark > 120)) {
      ui::arcPaint(fb, artLayer, fmaxf(0.001f, curFrac - 0.09f), m.frac, amt, spark,
                   bx0, by0, bx1, by1);
      if (!full) blitRect(bx0, by0, bx1, by1);
      curFrac = m.frac;
      lastSpark = now;
    }
  }

  if (m.digits && (full || m.secs != curSecs || m.dim != curDim)) {
    ui::drawTime(fb, artLayer, m.secs, amt);
    if (!full) blitRect(ui::DIG_X0, ui::DIG_Y0, ui::DIG_X1, ui::DIG_Y1);
    curSecs = m.secs;
  }

  float pulse = m.dim ? 0.55f + 0.45f * sinf(now * 0.006f) : 1.0f;
  float pq = floorf(pulse * 6.0f) / 6.0f;
  if (full || strcmp(m.lbl, curLbl) != 0 || pq != curPulseQ) {
    ui::drawLabel(fb, artLayer, m.lbl, 0.35f + 0.65f * pq);
    if (!full) blitRect(ui::LBL_X0, ui::LBL_Y0, ui::LBL_X1, ui::LBL_Y1);
    strncpy(curLbl, m.lbl, sizeof curLbl - 1);
    curLbl[sizeof curLbl - 1] = 0;
    curPulseQ = pq;
  }
  curDim = m.dim;
}

static void showScreen(const UiModel &m, uint32_t now) {
  // with digits the buddy scoots up and shrinks a bit so the time band is clear
  ui::expandArt(artLayer, ui::STATE_ART[m.scr].pal, ui::STATE_ART[m.scr].px,
                m.digits ? 0.72f : 1.0f, m.digits ? 42 : 0);
  ui::bakeArcTrack(artLayer);
  if (m.digits) ui::bakePill(artLayer, ui::DIG_X0 + 12, ui::DIG_Y0, ui::DIG_X1 - 12, ui::DIG_Y1, 0.68f);
  memcpy(gfx->getFramebuffer(), artLayer, (size_t)LCD_W * LCD_H * 2);
  curScr = m.scr;
  curDigits = m.digits;
  curLbl[0] = 0;
  curSecs = 0xFFFFFFFF;
  curFrac = -2;
  curPulseQ = -1;
  curDim = m.dim;
  paintDelta(m, now, true);
  gfx->flush();
  if (!backlightOn) setBacklight(true, backlightDim); // reveal after first real frame
  Serial.printf("[orb] screen -> %d (%s)\n", (int)m.scr, m.lbl);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  bootMs = millis();
  Serial.println("\n[orb] xTool buddy orb — JC3636W518C build " __DATE__ " " __TIME__);
  const esp_partition_t *part = esp_ota_get_running_partition();
  Serial.printf("[orb] running from %s @0x%06lx\n", part->label, (unsigned long)part->address);

  ledcAttach(BL_GPIO, 5000, 8);
  ledcWrite(BL_GPIO, 0); // dark until the first frame is up

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  touchReset();
  uint8_t chipId = 0;
  haveTouch = cstRead(0xA7, &chipId, 1) == 1;
  if (haveTouch) cstWrite(0xFE, 0x01); // disable auto-sleep so polling works
  Serial.printf("[orb] cst816 @0x15: %s (chip id 0x%02X)\n",
                haveTouch ? "OK, auto-sleep off" : "no response", chipId);

  havePanel = gfx->begin(LCD_QSPI_HZ);
  Serial.printf("[orb] st77916 qspi begin: %s\n", havePanel ? "OK" : "FAILED");
  if (havePanel) {
    gfx->fillScreen(RGB565_BLACK);
    gfx->flush();
  }

  artLayer = (uint16_t *)ps_malloc(LCD_W * LCD_H * 2);
  if (!artLayer) Serial.println("[orb] FATAL: no PSRAM for the art layer");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("xtool-orb");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[orb] wifi: connecting to %s\n", WIFI_SSID);
}

void loop() {
  uint32_t now = millis();
  if (now - lastFrame < 20) {
    delay(2);
    return;
  }
  lastFrame = now;

  static wl_status_t lastWifi = WL_IDLE_STATUS;
  static bool otaReady = false;
  wl_status_t wifiNow = WiFi.status();
  if (wifiNow != lastWifi) {
    lastWifi = wifiNow;
    if (wifiNow == WL_CONNECTED) {
      Serial.printf("[orb] wifi: connected, ip=%s\n", WiFi.localIP().toString().c_str());
      if (!otaReady) {
        ArduinoOTA.setHostname("xtool-orb");
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.begin();
        otaReady = true;
      }
      xtool::begin();
    } else {
      Serial.printf("[orb] wifi: status %d\n", wifiNow);
    }
  }
  if (otaReady) ArduinoOTA.handle();
  xtool::tick(now); // read-only S1 link; returns immediately unless wifi is up

  // tap toggles bright/dim backlight (release within 600 ms = a tap)
  if (touchPoll(touch) && !haveTouch) {
    haveTouch = true; // CST816 sleeps until first touch
    cstWrite(0xFE, 0x01);
  }
  if (touch.down && !wasDown) downAt = now;
  if (!touch.down && wasDown && now - downAt < 600 && backlightOn) {
    setBacklight(true, !backlightDim);
    Serial.printf("[orb] backlight: %s\n", backlightDim ? "dim" : "bright");
  }
  wasDown = touch.down;

  if (havePanel && artLayer) {
    UiModel m = pickModel(now);
    if (m.scr != curScr || m.digits != curDigits) showScreen(m, now);
    else paintDelta(m, now, false);
  }

  if (now - lastBeat > 5000) {
    lastBeat = now;
    Serial.printf("[orb] alive %lus scr=%d xt=S%d/%s%s heap=%u wifi=%d\n",
                  (unsigned long)((now - bootMs) / 1000), (int)curScr, xtool::rawStatus(),
                  xtool::PHASE_NAMES[xtool::phase()], xtool::linkUp() ? "" : " (no ws)",
                  ESP.getFreeHeap(), wifiNow == WL_CONNECTED);
  }
}
