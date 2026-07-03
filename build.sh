#!/usr/bin/env bash
# Build wrapper for stock meshtastic/firmware + the TCP bridge (port 4404)
# patch set. Modeled directly on meshcore-xiao-wifi-serial2tcp/build.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${WORK_DIR:-$SCRIPT_DIR/build}"
REPO_URL="${REPO_URL:-https://github.com/meshtastic/firmware.git}"
REPO_BRANCH="${REPO_BRANCH:-develop}"
CHECKOUT_DIR="$WORK_DIR/meshtastic-firmware"
PIO_ENV="${PIO_ENV:-}"

DO_CLONE=1
DO_PATCH=1
DO_BUILD=0
DO_UPLOAD=0
DO_MONITOR=0
UPLOAD_PORT="${UPLOAD_PORT:-}"

usage() {
  cat <<'EOF'
Usage: ./build.sh --env <platformio_env> --build [--upload] [OPTIONS]

Required:
    --env <name>   PlatformIO environment to build (e.g. heltec-v3, seeed-xiao-s3)
                    See: meshtastic-firmware/variants/*/platformio.ini after --no-build

Steps:
    --build        Clone/patch (unless skipped) and build firmware
    --upload       Upload previously built firmware (combine with --build to build+upload)

Options:
    --no-clone     Skip repository cloning (use existing checkout)
    --no-patch     Skip applying patches
    --monitor      Upload and start serial monitor
    --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --env) PIO_ENV="$2"; shift 2 ;;
    --build) DO_BUILD=1; shift ;;
    --upload) DO_UPLOAD=1; shift ;;
    --monitor) DO_MONITOR=1; shift ;;
    --no-clone) DO_CLONE=0; shift ;;
    --no-patch) DO_PATCH=0; shift ;;
    --help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ "$DO_BUILD" -eq 1 && -z "$PIO_ENV" ]]; then
  echo "error: --env is required with --build (e.g. --env heltec-v3)" >&2
  exit 1
fi

mkdir -p "$WORK_DIR"

if [[ "$DO_CLONE" -eq 1 ]]; then
  if [[ -d "$CHECKOUT_DIR/.git" ]]; then
    echo "Checkout already exists at $CHECKOUT_DIR, skipping clone (use --no-clone to silence this)"
  else
    git clone --branch "$REPO_BRANCH" "$REPO_URL" "$CHECKOUT_DIR"
  fi
fi

if [[ "$DO_PATCH" -eq 1 ]]; then
  for p in "$SCRIPT_DIR"/patches/*.patch; do
    echo "Applying $(basename "$p")"
    (cd "$CHECKOUT_DIR" && git apply --check "$p") || {
      echo "error: $p does not apply cleanly against the current checkout" >&2
      echo "       (upstream firmware may have moved on -- rebase the patch by hand)" >&2
      exit 1
    }
    (cd "$CHECKOUT_DIR" && git apply "$p")
  done
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  command -v pio >/dev/null 2>&1 || {
    echo "error: platformio (pio) not found in PATH. Install with:" >&2
    echo "       python3 -m pip install --user platformio" >&2
    exit 1
  }
  (cd "$CHECKOUT_DIR" && pio run -e "$PIO_ENV")
fi

if [[ "$DO_UPLOAD" -eq 1 ]]; then
  UPLOAD_ARGS=()
  [[ -n "$UPLOAD_PORT" ]] && UPLOAD_ARGS+=(--upload-port "$UPLOAD_PORT")
  (cd "$CHECKOUT_DIR" && pio run -e "$PIO_ENV" -t upload "${UPLOAD_ARGS[@]}")
fi

if [[ "$DO_MONITOR" -eq 1 ]]; then
  (cd "$CHECKOUT_DIR" && pio device monitor)
fi

echo "Done. Firmware (if built): $CHECKOUT_DIR/.pio/build/$PIO_ENV/firmware.bin"
