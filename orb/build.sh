#!/usr/bin/env bash
# Compile (and optionally flash) the xTool buddy orb.
#   ./build.sh            compile only
#   ./build.sh flash      compile + upload over USB (/dev/ttyACM0)
#   ./build.sh ota [host] compile + upload over Wi-Fi (default xtool-orb.local)
#   ./build.sh monitor    serial monitor
# Flash safety on this board: never reassign GPIO19/20 (native USB D-/D+) — a
# sketch that touches them knocks the unit off the USB bus on every boot, and
# the battery keeps it running; that is exactly how unit #1 "bricked".
set -eu
cd "$(dirname "$0")"
PORT=${ORB_PORT:-/dev/ttyACM0}
# S3R8: 16MB quad flash (eFuse-confirmed qio), 8MB octal PSRAM (opi).
# default_16MB keeps two OTA app slots so a mounted orb reflashes over Wi-Fi.
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,USBMode=hwcdc"

[ -f secrets.h ] || { echo "missing secrets.h — cp secrets.h.example secrets.h and fill it in"; exit 1; }

case "${1:-compile}" in
  compile) arduino-cli compile --build-property compiler.optimization_flags=-O2 --fqbn "$FQBN" . ;;
  flash)   arduino-cli compile --build-property compiler.optimization_flags=-O2 --fqbn "$FQBN" .
           arduino-cli upload -p "$PORT" --fqbn "$FQBN" . ;;
  ota)     arduino-cli compile --build-property compiler.optimization_flags=-O2 --fqbn "$FQBN" --export-binaries .
           ESPOTA=$(ls "$HOME"/.arduino15/packages/esp32/hardware/esp32/*/tools/espota.py | head -1)
           AUTH=$(grep OTA_PASSWORD secrets.h | cut -d'"' -f2)
           python3 "$ESPOTA" -i "${2:-xtool-orb.local}" -p 3232 --auth="$AUTH" \
             -f build/esp32.esp32.esp32s3/orb.ino.bin ;;
  monitor) exec arduino-cli monitor -p "$PORT" -c baudrate=115200 ;;
  *) echo "usage: $0 [compile|flash|ota [host]|monitor]"; exit 1 ;;
esac
