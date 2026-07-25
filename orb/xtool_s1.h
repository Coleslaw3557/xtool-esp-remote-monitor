// xTool S1 link — auto-discovers the laser on the LAN and mirrors its job
// state so the orb can carve a cut timer.
//
// Protocol (community-documented — ha-xtool PROTOCOL.md, BassXT/xtool,
// 1RandomDev/xTool-Connect — and verified live against this S1 on firmware
// V40.32.013.2224.01, 2026-07-24):
//   discovery  broadcast {"ip","port","requestId"} JSON to UDP :20000; the
//              machine unicasts back {ip, name:"xTool S1", version}. (D1-era
//              units may try TCP :20001 first and delay the UDP reply ~20 s;
//              this S1 answered over UDP within milliseconds, so no TCP
//              acceptor — worst case discovery costs one extra probe cycle.)
//   status     plain (non-TLS) WebSocket ws://<ip>:8081/ carrying one-line
//              ASCII M-codes. The machine streams M303 head positions ~2 Hz
//              (free liveness), answers "M222\n" with "M222 S<n>", and pushes
//              M810 "<file>" / M815 T<sec> around job load. "M2003\n" dumps
//              device-info JSON (includes the current M222 state) — sent once
//              per connect so the boot log names the machine.
//
// WIRE SAFETY: the same socket accepts control writes. This file only ever
// transmits the read-only queries M222 / M2003 (plus WS pong). NEVER add
// M341 S1 (locks the S1 into wifi-setup until power-cycled) or M9006
// (crashes/reboots it), and don't probe undocumented /system HTTP actions.
// The S1 serves ONE ws client at a time: opening XCS on a desktop evicts us;
// we just keep redialing and win the slot back when XCS lets go.
//
// M222 S-codes: 0 boot, 1/3 idle, 2 wifi-setup, 10/24 measuring, 11 frame-
// ready, 12 framing, 13 job-ready, 14 processing, 15 paused, 16 fw-update,
// 17 sleeping, 18 cancelling, 19 finished. Error family, per xTool's own
// support articles (support.xtool.com/article/1088, 1277, 1278, 1279):
//   4 device moved, 7 started with lid open (or no job file), 9 FLAME
//   detected, 20 hit position limit mid-job, 21 wifi-module<->controller
//   comms fault, 22 laser-module<->controller comms fault.
#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>

#ifndef XTOOL_FALLBACK_IP
#define XTOOL_FALLBACK_IP "" // dialed slowly even if discovery never fires
#endif

namespace xtool {

enum Phase : uint8_t { PH_NONE, PH_READY, PH_FRAMING, PH_RUNNING, PH_PAUSED, PH_DONE };

constexpr uint16_t DISC_PORT = 20000;
constexpr uint16_t DISC_LOCAL_PORT = 47788;
constexpr uint16_t WS_PORT = 8081;
constexpr uint32_t PROBE_PERIOD_MS = 15000; // discovery cadence while unlinked
constexpr uint32_t SIGHT_FRESH_MS = 90000;  // reply this recent -> machine is up
constexpr uint32_t DIAL_FAST_MS = 5000;     // ws retry while machine is known up
constexpr uint32_t DIAL_SLOW_MS = 60000;    // blind attempts on a stale/fallback IP
constexpr uint32_t POLL_MS = 1000;          // M222 cadence
constexpr uint32_t RX_STALE_MS = 6000;      // no frames (not even M303) -> dead
constexpr uint32_t HANDSHAKE_MS = 4000;     // http 101 must land this fast
constexpr uint32_t DONE_HOLD_MS = 60000;    // linger on DONE before the face returns
constexpr uint32_t JOB_GIVEUP_MS = 600000;  // dark this long mid-job -> forget it

static WiFiUDP udp;
static WiFiClient ws;
static bool started = false;
static char devIp[16] = XTOOL_FALLBACK_IP;
static char devName[24] = "";
static uint32_t lastSeen = 0, lastProbe = 0, lastDial = 0, lastPoll = 0, lastRx = 0;
static bool sockUp = false, wsUp = false; // tcp connected / 101 handshake done
static uint8_t rxBuf[1600];
static size_t rxLen = 0;
static char lineBuf[900]; // M2003 info dumps run ~600 bytes on one line
static size_t lineLen = 0;

static int status = -1;     // last M222 S-code seen
static int totalSecRaw = 0; // last M815 T value (gated by totalSec())
static char jobName[40] = "";
static bool lostLink = false; // ws died while a job phase was live
static Phase ph = PH_NONE;
static uint32_t runStart = 0, freezeAt = 0, pausedAccum = 0, doneAt = 0;
static bool clockRun = false;

static bool linkUp() { return wsUp; }
static Phase phase() { return ph; }
static int rawStatus() { return status; }
static const char *ip() { return devIp; }

// Elapsed job time as this orb witnessed it. If the orb links up mid-cut the
// count starts from the link, not the true job start — the S1 exposes no
// elapsed-time query to recover it. Link outages do NOT freeze the clock (the
// laser keeps cutting whether or not we can see it).
static uint32_t elapsedMs(uint32_t now) {
  if (!runStart) return 0;
  uint32_t end = clockRun ? now : freezeAt;
  uint32_t e = end - runStart;
  return e > pausedAccum ? e - pausedAccum : 0;
}

// M815 is community-documented as the job time in seconds but hasn't been
// captured on this firmware yet — gate it so a bogus value can only degrade
// the countdown into the count-up it would have been anyway.
static int totalSec(uint32_t now) {
  if (totalSecRaw < 10 || totalSecRaw > 86400) return 0;
  if (elapsedMs(now) / 1000 > (uint32_t)(totalSecRaw + totalSecRaw / 8 + 20)) return 0;
  return totalSecRaw;
}

static const char *const PHASE_NAMES[] = {"none", "ready", "framing", "running", "paused", "done"};

static void setPhase(int s, uint32_t now) {
  Phase np;
  switch (s) {
    case 10: case 11: case 13: case 24: np = PH_READY; break; // measuring rides READY
    case 12: np = PH_FRAMING; break;
    case 14: np = PH_RUNNING; break;
    case 15: np = PH_PAUSED; break;
    case 19: np = runStart ? PH_DONE : PH_NONE; break; // stale "finished" at boot: no scene
    case 0: case 1: case 3: case 17:
      // idle right after a live run = the machine skipped/left "finished" fast
      np = (ph == PH_RUNNING || ph == PH_PAUSED || ph == PH_DONE) ? PH_DONE : PH_NONE;
      break;
    default: np = PH_NONE; break; // cancelling, errors, fw update, wifi setup
  }
  if (np == ph) return;
  if (np == PH_RUNNING || np == PH_FRAMING) {
    if (ph == PH_PAUSED && np == PH_RUNNING) {
      pausedAccum += now - freezeAt; // resume: the pause gap doesn't count
    } else {
      runStart = now; // fresh job (or framing pass)
      pausedAccum = 0;
    }
    clockRun = true;
  } else if (np == PH_PAUSED) {
    freezeAt = now;
    clockRun = false;
  } else if (np == PH_DONE) {
    if (clockRun) freezeAt = now;
    doneAt = now;
    clockRun = false;
  } else {
    clockRun = false;
    if (np == PH_READY) { runStart = 0; pausedAccum = 0; } // arm for a new job
  }
  Serial.printf("[xtool] S%d: %s -> %s\n", s, PHASE_NAMES[ph], PHASE_NAMES[np]);
  ph = np;
}

static void handleLine(char *l, uint32_t now) {
  if (!strncmp(l, "M303", 4)) return; // head position stream: liveness only
  if (!strncmp(l, "M222 S", 6)) {
    int s = atoi(l + 6);
    lostLink = false;
    if (s != status) {
      status = s;
      setPhase(s, now);
    }
    return;
  }
  if (!strncmp(l, "M810", 4)) { // job filename, quoted on most firmware
    const char *a = strchr(l, '"');
    const char *b = a ? strrchr(l, '"') : nullptr;
    if (a && b && b > a + 1) {
      size_t n = (size_t)(b - a - 1);
      if (n >= sizeof(jobName)) n = sizeof(jobName) - 1;
      memcpy(jobName, a + 1, n);
      jobName[n] = 0;
    }
    Serial.printf("[xtool] job: %s\n", jobName);
    return;
  }
  if (!strncmp(l, "M815 T", 6)) {
    totalSecRaw = atoi(l + 6);
    Serial.printf("[xtool] job time: %ds\n", totalSecRaw);
    return;
  }
  if (!strncmp(l, "M2003", 5)) { // info dump; carries current state + model name
    const char *p = strstr(l, "\"M222\":\"S");
    if (p) {
      int s = atoi(p + 9);
      lostLink = false;
      if (s != status) {
        status = s;
        setPhase(s, now);
      }
    }
    const char *n = strstr(l, "\"M100\":\"");
    if (n) {
      n += 8;
      size_t i = 0;
      while (n[i] && n[i] != '"' && i < sizeof(devName) - 1) { devName[i] = n[i]; i++; }
      devName[i] = 0;
    }
    Serial.printf("[xtool] hello %s @ %s\n", devName[0] ? devName : "?", devIp);
    return;
  }
}

// client->server frames must be masked (RFC 6455); a fixed mask is fine
static void wsTx(const char *cmd) {
  size_t n = strlen(cmd);
  if (n > 60) return;
  uint8_t f[70] = {0x81, (uint8_t)(0x80 | n), 'L', 'O', 'H', 'P'};
  for (size_t i = 0; i < n; i++) f[6 + i] = cmd[i] ^ f[2 + (i & 3)];
  ws.write(f, 6 + n);
}

static void dropWs(const char *why) {
  if (sockUp) Serial.printf("[xtool] ws down (%s)\n", why);
  ws.stop();
  sockUp = wsUp = false;
  rxLen = lineLen = 0;
  if (ph != PH_NONE && ph != PH_DONE) lostLink = true; // keep the frozen scene honest
}

static void pumpWs(uint32_t now) {
  while (ws.available()) {
    int room = (int)sizeof(rxBuf) - (int)rxLen;
    if (room <= 0) { dropWs("rx overflow"); return; }
    int got = ws.read(rxBuf + rxLen, room);
    if (got <= 0) break;
    rxLen += got;
    lastRx = now;
  }
  if (!wsUp) { // waiting on the http 101
    for (size_t i = 3; i < rxLen; i++) {
      if (memcmp(rxBuf + i - 3, "\r\n\r\n", 4)) continue;
      rxBuf[i - 3] = 0;
      if (!strstr((char *)rxBuf, " 101")) { dropWs("no 101"); return; }
      memmove(rxBuf, rxBuf + i + 1, rxLen - (i + 1));
      rxLen -= i + 1;
      wsUp = true;
      lastPoll = now;
      Serial.printf("[xtool] ws up: %s:%d\n", devIp, WS_PORT);
      wsTx("M2003\n"); // logs the model + seeds status without waiting a poll
      break;
    }
    if (!wsUp) {
      if (!ws.connected() && !ws.available()) dropWs("closed in handshake");
      return;
    }
  }
  for (;;) { // tiny unfragmented text frames; server sends unmasked
    if (rxLen < 2) break;
    uint8_t op = rxBuf[0] & 0x0F;
    bool masked = rxBuf[1] & 0x80;
    size_t ln = rxBuf[1] & 0x7F, idx = 2;
    if (ln == 126) {
      if (rxLen < 4) break;
      ln = ((size_t)rxBuf[2] << 8) | rxBuf[3];
      idx = 4;
    } else if (ln == 127) {
      dropWs("jumbo frame");
      return;
    }
    uint8_t mk[4] = {0, 0, 0, 0};
    if (masked) {
      if (rxLen < idx + 4) break;
      memcpy(mk, rxBuf + idx, 4);
      idx += 4;
    }
    if (idx + ln > sizeof(rxBuf)) { dropWs("frame too big"); return; }
    if (rxLen < idx + ln) break;
    if (op == 0x9) {
      uint8_t pong[6] = {0x8A, 0x80, 'L', 'O', 'H', 'P'};
      ws.write(pong, 6);
    } else if (op == 0x8) {
      dropWs("close frame");
      return;
    } else if (op == 0x1 || op == 0x2 || op == 0x0) {
      for (size_t i = 0; i < ln; i++) {
        char c = (char)(rxBuf[idx + i] ^ mk[i & 3]);
        if (c == '\n' || lineLen >= sizeof(lineBuf) - 1) {
          lineBuf[lineLen] = 0;
          if (lineLen) handleLine(lineBuf, now);
          lineLen = 0;
          if (c != '\n' && c != '\r') lineBuf[lineLen++] = c;
        } else if (c != '\r') {
          lineBuf[lineLen++] = c;
        }
      }
    }
    memmove(rxBuf, rxBuf + idx + ln, rxLen - (idx + ln));
    rxLen -= idx + ln;
  }
  if (!ws.connected() && !ws.available() && rxLen < 2) dropWs("closed");
}

static void probe(uint32_t now) {
  lastProbe = now;
  char buf[120];
  snprintf(buf, sizeof buf, "{\"ip\":\"%s\",\"port\":%u,\"requestId\":%lu}",
           WiFi.localIP().toString().c_str(), DISC_LOCAL_PORT, (unsigned long)now);
  udp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
  udp.write((const uint8_t *)buf, strlen(buf));
  udp.endPacket();
}

// copies the string value of "key" (reply JSON is tab-formatted) into dst
static bool jsonStr(const char *body, const char *key, char *dst, size_t cap) {
  char pat[16];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(body, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  while (*p && *p != '"') p++;
  if (!*p) return false;
  p++;
  size_t i = 0;
  while (p[i] && p[i] != '"' && i < cap - 1) { dst[i] = p[i]; i++; }
  dst[i] = 0;
  return i > 0;
}

static void pumpUdp(uint32_t now) {
  while (udp.parsePacket() > 0) {
    char b[360];
    int got = udp.read(b, sizeof b - 1);
    if (got <= 0) continue;
    b[got] = 0;
    char nm[24], ipStr[16];
    // replies carry "name"; probes (ours and XCS's) don't — that filters both
    if (!jsonStr(b, "name", nm, sizeof nm) || !jsonStr(b, "ip", ipStr, sizeof ipStr)) continue;
    if (!strcmp(ipStr, WiFi.localIP().toString().c_str())) continue;
    bool moved = strcmp(ipStr, devIp) != 0;
    if (moved || !lastSeen)
      Serial.printf("[xtool] discovered \"%s\" at %s\n", nm, ipStr);
    strncpy(devIp, ipStr, sizeof devIp - 1);
    devIp[sizeof devIp - 1] = 0;
    lastSeen = now;
    if (moved) dropWs("machine moved");
  }
}

static void begin() {
  if (started) return;
  udp.begin(DISC_LOCAL_PORT);
  started = true;
  Serial.println("[xtool] link task up, probing :20000");
}

static void tick(uint32_t now) {
  if (!started) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (sockUp) dropWs("wifi lost"); // else stale wsUp keeps "NO LINK" off screen
    return;
  }
  pumpUdp(now);
  if (!sockUp && (!lastProbe || now - lastProbe >= PROBE_PERIOD_MS)) probe(now);
  bool fresh = lastSeen && now - lastSeen < SIGHT_FRESH_MS;
  if (!sockUp && devIp[0]) {
    // dial fast only on a recent sighting: a blind connect() to a dark IP
    // blocks the face loop for its full 400 ms timeout
    uint32_t wait = fresh ? DIAL_FAST_MS : DIAL_SLOW_MS;
    if (!lastDial || now - lastDial >= wait) {
      lastDial = now;
      if (ws.connect(devIp, WS_PORT, 400)) {
        ws.setNoDelay(true);
        char req[200];
        snprintf(req, sizeof req,
                 "GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: bG9ocC1jdWRkbGUtb3JiMQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n\r\n",
                 devIp, WS_PORT);
        ws.print(req);
        sockUp = true;
        wsUp = false;
        rxLen = lineLen = 0;
        lastRx = now;
      } else {
        ws.stop();
      }
    }
  }
  if (sockUp) pumpWs(now);
  if (sockUp && now - lastRx > RX_STALE_MS) dropWs("stale");
  if (sockUp && !wsUp && now - lastDial > HANDSHAKE_MS) dropWs("handshake timeout");
  if (wsUp && now - lastPoll >= POLL_MS) {
    lastPoll = now;
    wsTx("M222\n");
  }
  if (ph == PH_DONE && now - doneAt > DONE_HOLD_MS) {
    ph = PH_NONE; // release the face; forget the finished job
    runStart = 0;
    totalSecRaw = 0;
    jobName[0] = 0;
  }
  if (lostLink && now - lastRx > JOB_GIVEUP_MS) {
    Serial.println("[xtool] dark too long mid-job, forgetting it");
    lostLink = false;
    ph = PH_NONE;
    runStart = 0;
    totalSecRaw = 0;
  }
}

} // namespace xtool
