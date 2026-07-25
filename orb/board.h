// Guition JC3636W518C — 1.8" round IPS 360x360, ESP32-S3R8 (16MB quad flash /
// 8MB octal PSRAM), ST77916 QSPI panel, CST816 touch, QI wireless power (no
// battery, no IMU — official spec + teardown; shake/dock gestures have no
// onboard sensor).
//
// THIS UNIT IS THE ORIGINAL 2024 WIRING — proven electrically 2026-07-23 by
// firmware/orb-diag: CST816 ACKs at 0x15 on SDA=7/SCL=8 after a GPIO40 reset
// pulse; the newer "expander" wiring's PCA9554 @0x20 is absent on 11/10. The
// CLK=40/data 46,45,42,41/CS=21/PCA9554 map in vendor threads is the mid-2025
// PCB revision, and "CS=14" is the knob sibling JC3636K518 — neither is this
// board. Pin map below matches the official schematic pincfg.h and the
// Arduino_GFX 1.6.7 bundled JC3636W518 profile.
#pragma once

// ST77916 panel over QSPI — 40MHz (panel specced 50; 80 reported unstable)
#define LCD_QSPI_CS 10
#define LCD_QSPI_CLK 9
#define LCD_QSPI_D0 11
#define LCD_QSPI_D1 12
#define LCD_QSPI_D2 13
#define LCD_QSPI_D3 14
#define LCD_RST 47
#define LCD_QSPI_HZ 40000000
#define LCD_W 360
#define LCD_H 360

// Backlight: AO3400A N-MOSFET on GPIO15, active high, PWM-able
#define BL_GPIO 15

// CST816 touch: own I2C bus + reset/interrupt GPIOs. The chip NACKs while
// asleep; it reliably ACKs in a window right after a reset pulse — wake it
// there and write 0xFE (disable auto-sleep) so polling works from boot.
#define I2C_SDA 7
#define I2C_SCL 8
#define CST816_ADDR 0x15
#define TOUCH_RST 40
#define TOUCH_INT 41

// Also on this wiring, unused by the orb: SD/MMC on GPIO1-6; PCM5100A line-out
// DAC LRCK=16 DIN=17 BCK=18 XSMT=48 (drive 48 HIGH to unmute if audio is ever
// wanted); I2S mic SCK=42 WS=45 SD=46; K1 user/boot button on GPIO0 (hold at
// power-on = ROM download mode — the unbrick path).

// Mirror the pupil's horizontal tracking if it reads reversed on the real
// panel (sim plan doc: "gaze sign" TBD).
#define GAZE_FLIP_X 0
