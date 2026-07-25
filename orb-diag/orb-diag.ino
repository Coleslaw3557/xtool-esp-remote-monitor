// JC3636W518 wiring-generation prober. The W518 family shipped with two PCB
// wirings; serial output alone names which one this unit is:
//   NEW (mid-2025+): I2C SDA11/SCL10 with PCA9554 @0x20 (always ACKs),
//                    QSPI 40/46/45/42/41 CS21, LCD reset behind expander.
//   OLD (2024):      touch I2C SDA7/SCL8 (CST816 @0x15 — sleeps, but ACKs for a
//                    window right after its reset line GPIO40 is pulsed),
//                    QSPI CS10/SCK9/D0-3 11..14, LCD RST 47, backlight 15.
// No display init here — scans only, so no bus claims mask the probes.
// GPIO19/20 (USB) are never touched.
#include <Wire.h>

static int scanPair(int sda, int scl, const char *tag) {
  Wire.end();
  if (!Wire.begin(sda, scl, 100000)) {
    Serial.printf("  %s (SDA=%d SCL=%d): Wire.begin failed\n", tag, sda, scl);
    return 0;
  }
  delay(5);
  int hits = 0;
  String found;
  for (uint8_t a = 8; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char b[8];
      snprintf(b, sizeof(b), "0x%02X ", a);
      found += b;
      hits++;
    }
  }
  Serial.printf("  %s (SDA=%d SCL=%d): %s\n", tag, sda, scl,
                hits ? found.c_str() : "silent");
  Wire.end();
  return hits;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[diag] JC3636W518 wiring prober " __DATE__ " " __TIME__);
  pinMode(0, INPUT_PULLUP);  // K1 boot/user button
  pinMode(18, INPUT);        // TE candidate on new wiring (informational)
}

void loop() {
  Serial.println("[diag] --- probe round ---");

  // OLD-wiring beacon: wake CST816 via its GPIO40 reset, catch its post-reset
  // ACK window on 7/8. GPIO40 floats harmlessly on new wiring (QSPI CLK, idle).
  pinMode(40, OUTPUT);
  digitalWrite(40, LOW);
  delay(8);
  digitalWrite(40, HIGH);
  delay(150);
  int oldHits = scanPair(7, 8, "old-wiring bus") + scanPair(8, 7, "old-wiring swapped");

  // NEW-wiring beacon: PCA9554 @0x20 is always powered, always ACKs.
  int newHits = scanPair(11, 10, "new-wiring bus") + scanPair(10, 11, "new-wiring swapped");

  int te = 0, last = digitalRead(18);
  uint32_t until = millis() + 200;
  while (millis() < until) {
    int v = digitalRead(18);
    if (v != last) te++;
    last = v;
  }

  Serial.printf("[diag] button GPIO0=%s  TE(18) edges/200ms=%d\n",
                digitalRead(0) ? "up" : "PRESSED", te);
  if (newHits && !oldHits) Serial.println("[diag] VERDICT: NEW wiring (expander)");
  else if (oldHits && !newHits) Serial.println("[diag] VERDICT: OLD wiring (2024)");
  else if (!oldHits && !newHits)
    Serial.println("[diag] VERDICT: nothing ACKed — hold a finger on the glass "
                   "next round to wake a sleepy CST816");
  else Serial.println("[diag] VERDICT: ambiguous — both buses ACKed?!");
  delay(6000);
}
