#!/usr/bin/env bash
#
# flash-unit.sh — provision a Vibe Radio unit.
#
#   ./tools/flash-unit.sh              # new unit: bootloader + tables + app + web UI, NVS wiped
#   ./tools/flash-unit.sh --update     # existing unit: app only, keeps WiFi and settings
#   ./tools/flash-unit.sh --update --fs # app + web UI, keeps WiFi and settings
#
# Every guard here exists because the thing it guards against actually happened
# during development. See NOTES.md in the parent directory.

set -euo pipefail

ENV_NAME="${PIO_ENV:-tembed}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/.pio/build/$ENV_NAME"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL=("$HOME/.platformio/penv/bin/python" -m esptool)

# Offsets are read back from the device in --update mode; these are the values the
# partition table generates for a 16MB board.
APP_OFFSET=0x20000
FS_OFFSET=0x620000
NVS_OFFSET=0x9000
NVS_SIZE=0x10000

MODE="new"
FLASH_FS="yes"
for arg in "$@"; do
  case "$arg" in
    --update) MODE="update"; FLASH_FS="no" ;;
    --fs)     FLASH_FS="yes" ;;
    --help|-h) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!  %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mXX  %s\033[0m\n' "$*" >&2; exit 1; }

# ── 1. Find the port ─────────────────────────────────────────────────────────
say "Looking for a board"
PORTS=(/dev/cu.usbmodem*)
[ -e "${PORTS[0]}" ] || die "No /dev/cu.usbmodem* found. Plug the unit in via USB."
[ "${#PORTS[@]}" -eq 1 ] ||
  die "More than one board connected: ${PORTS[*]}. Provision one at a time."
PORT="${PORTS[0]}"
echo "    $PORT"

# ── 2. Nothing else may hold the port ────────────────────────────────────────
# A leftover `cat /dev/cu.*` from a log capture eats esptool's replies and
# produces six identical mid-write failures that look exactly like a bad cable.
if HOLDER=$(lsof -t "$PORT" 2>/dev/null) && [ -n "$HOLDER" ]; then
  echo "    held by:"; ps -o pid,command -p "$HOLDER" | tail -n +2 | sed 's/^/      /'
  die "Something has $PORT open. Close it (serial monitor, log capture) and retry."
fi
echo "    port is free"

# ── 3. Build, and refuse to flash a stale binary ─────────────────────────────
say "Building ($ENV_NAME)"
"$PIO" run -e "$ENV_NAME" >/tmp/vibe-build.log 2>&1 ||
  { tail -30 /tmp/vibe-build.log; die "Build failed. Nothing flashed."; }
grep -E '^(RAM|Flash):' /tmp/vibe-build.log | sed 's/^/    /' || true

if [ "$FLASH_FS" = "yes" ]; then
  say "Building web UI image"
  "$PIO" run -e "$ENV_NAME" -t buildfs >/tmp/vibe-fs.log 2>&1 ||
    { tail -30 /tmp/vibe-fs.log; die "SPIFFS build failed. Nothing flashed."; }
fi

APP="$BUILD_DIR/firmware.bin"
FS_IMG="$BUILD_DIR/spiffs.bin"
[ -f "$APP" ] || die "Missing $APP"
[ "$FLASH_FS" = "no" ] || [ -f "$FS_IMG" ] || die "Missing $FS_IMG"

# ── 4. Write ─────────────────────────────────────────────────────────────────
COMMON=(--chip esp32s3 --port "$PORT" --baud 921600
        --before default-reset --after hard-reset)
FLASH_ARGS=(--flash-mode dio --flash-size 16MB --flash-freq 80m)

if [ "$MODE" = "new" ]; then
  say "Provisioning a NEW unit (full flash, settings wiped)"
  "${ESPTOOL[@]}" "${COMMON[@]}" write-flash "${FLASH_ARGS[@]}" \
    0x0      "$BUILD_DIR/bootloader.bin" \
    0x8000   "$BUILD_DIR/partitions.bin" \
    0x19000  "$BUILD_DIR/ota_data_initial.bin" \
    "$APP_OFFSET" "$APP" \
    "$FS_OFFSET"  "$FS_IMG" 2>&1 | grep -viE '^warning: deprecated' |
      grep -E 'Writing|Wrote|Hash|error' | sed 's/^/    /'

  # A fresh board carries whatever NVS the factory or a previous life left. Wipe
  # it so the unit boots unprovisioned and shows the setup screen.
  say "Erasing settings (NVS)"
  "${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
    erase-region "$NVS_OFFSET" "$NVS_SIZE" 2>&1 |
      grep -viE '^warning' | grep -E 'erased|error' | sed 's/^/    /'
else
  say "Updating an existing unit (settings and WiFi kept)"
  # Confirm the offsets against the device rather than trusting a constant: a
  # wrong offset writes over another partition.
  PT=/tmp/vibe-pt.bin
  "${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
    read-flash 0x8000 0xc00 "$PT" >/dev/null 2>&1 || die "Could not read the partition table"
  GEN="$HOME/.platformio/packages/framework-espidf/components/partition_table/gen_esp32part.py"
  if [ -f "$GEN" ]; then
    python3 "$GEN" "$PT" 2>/dev/null | sed 's/^/    /'
    python3 "$GEN" "$PT" 2>/dev/null | grep -q ",app,ota_0,$APP_OFFSET," ||
      die "Device app partition is not at $APP_OFFSET — use a full flash instead."
  fi

  ARGS=("$APP_OFFSET" "$APP")
  [ "$FLASH_FS" = "no" ] || ARGS+=("$FS_OFFSET" "$FS_IMG")
  "${ESPTOOL[@]}" "${COMMON[@]}" write-flash "${FLASH_ARGS[@]}" "${ARGS[@]}" 2>&1 |
    grep -viE '^warning: deprecated' | grep -E 'Wrote|Hash|error' | sed 's/^/    /'
fi

# ── 5. Verify what is actually on the chip ───────────────────────────────────
say "Verifying"
"${ESPTOOL[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
  verify-flash "$APP_OFFSET" "$APP" 2>&1 | grep -viE '^warning' |
    grep -E 'Verification|error' | sed 's/^/    /' ||
  die "Verification failed — do not ship this unit."

# ── 6. The bit everyone forgets ──────────────────────────────────────────────
cat <<'DONE'

    ────────────────────────────────────────────────────────────
    Done. NOW UNPLUG THE UNIT AND PLUG IT BACK IN.

    The reset esptool just performed is a WARM reset, and this
    board does not boot from one — it will sit dark and look
    broken until it gets a cold power cycle.

    Expected after replugging:
      1. VIBE / RADIO splash
      2. "Join WiFi" + the setup network name  (new unit)
         or the station playing                (already provisioned)
    ────────────────────────────────────────────────────────────

DONE
