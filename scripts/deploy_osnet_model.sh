#!/usr/bin/env bash
# Fetch the OSNet x1.0 ONNX model and place it where ReIDEngine looks
# for it on startup (cpp-backend/models/osnet_x1_0.onnx). Bash flavour
# of scripts/deploy_osnet_model.ps1 — same interface, same idempotency.
#
# Without the file present:
#   - ReIDEngine::init() falls through all four candidate paths and
#     logs "No ONNX model found. Running in hash-based fallback mode
#     (reduced accuracy)." (cpp-backend/src/core/reid_engine.cpp:132)
#   - The engine still works — gallery + trails + persistence (the
#     2026-05-14 reid_persistence pass) all function — but match
#     accuracy is lower because the embedding is a 128-bin color
#     histogram, not a learned 512-dim feature vector.
#
# Where to obtain a valid OSNet x1.0 ONNX:
#   1. Self-export from torchreid (https://kaiyangzhou.github.io/deep-person-reid/):
#        pip install torchreid
#        # Then in Python:
#        #   import torchreid, torch
#        #   m = torchreid.models.build_model('osnet_x1_0', num_classes=1000, pretrained=True)
#        #   m.eval()
#        #   torch.onnx.export(m, torch.randn(1,3,256,128), 'osnet_x1_0.onnx',
#        #                     input_names=['input'], output_names=['features'],
#        #                     dynamic_axes={'input':{0:'batch'}, 'features':{0:'batch'}},
#        #                     opset_version=11)
#      This produces the canonical (1,3,256,128)->(1,512) network the
#      backend expects (resize/blob path in reid_engine.cpp:395 uses
#      cv::Size(128, 256), matching the H=256/W=128 OSNet input).
#   2. A community pre-export (e.g. yolo_tracking releases). Trust the
#      upstream maintainer; pin via --sha256 below.
#   3. An internal artifact server. Pass --url <https url> + --sha256.
#
# Usage:
#   export OSNET_MODEL_URL=https://example.com/osnet_x1_0.onnx
#   export OSNET_MODEL_SHA256=abc123...   # optional but recommended
#   scripts/deploy_osnet_model.sh
#
#   # or with explicit args:
#   scripts/deploy_osnet_model.sh --url https://... --sha256 abc...
#
#   # force re-download even if file already exists and matches:
#   scripts/deploy_osnet_model.sh --force
#
# Idempotent: if the target file exists and (a) no --sha256 was given,
# or (b) the existing file matches the given --sha256, the script
# returns successfully without re-downloading. No service restart is
# triggered by this script — the model is loaded once at backend boot,
# so the operator restarts the service after deploying a new model.

set -euo pipefail

URL="${OSNET_MODEL_URL:-}"
SHA256="${OSNET_MODEL_SHA256:-}"
FORCE=0
MODEL_NAME="osnet_x1_0.onnx"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --url)        URL="$2"; shift 2;;
        --sha256)     SHA256="$2"; shift 2;;
        --force)      FORCE=1; shift;;
        --model-name) MODEL_NAME="$2"; shift 2;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0;;
        *) echo "Unknown arg: $1" >&2; exit 2;;
    esac
done

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)
MODELS_DIR="$REPO_ROOT/cpp-backend/models"
TARGET="$MODELS_DIR/$MODEL_NAME"

# Pick a sha256 tool. macOS ships `shasum`; Linux ships `sha256sum`. busybox has neither in some images.
sha256_of() {
    local path="$1"
    if command -v sha256sum &>/dev/null; then
        sha256sum "$path" | awk '{print tolower($1)}'
    elif command -v shasum &>/dev/null; then
        shasum -a 256 "$path" | awk '{print tolower($1)}'
    else
        echo "ERROR: neither sha256sum nor shasum found on PATH; cannot verify integrity." >&2
        return 1
    fi
}

# ── Idempotency check ────────────────────────────────────────────────────
existing_matches=0
if [[ -f "$TARGET" ]]; then
    if [[ $FORCE -eq 1 ]]; then
        echo "[osnet-deploy] --force given; ignoring existing file at $TARGET"
    elif [[ -z "$SHA256" ]]; then
        size_mb=$(du -m "$TARGET" | awk '{print $1}')
        echo "[osnet-deploy] $TARGET already exists (${size_mb} MB). No --sha256 given, skipping re-download."
        echo "[osnet-deploy] Pass --force to re-download or --sha256 to verify integrity."
        existing_matches=1
    else
        actual=$(sha256_of "$TARGET")
        expected="${SHA256,,}"
        if [[ "$actual" == "$expected" ]]; then
            echo "[osnet-deploy] $TARGET already present with matching sha256, nothing to do."
            existing_matches=1
        else
            echo "[osnet-deploy] Existing file sha256 mismatch (have $actual, want $expected). Will re-download."
        fi
    fi
fi

[[ $existing_matches -eq 1 ]] && exit 0

# ── Validate URL ────────────────────────────────────────────────────────
if [[ -z "$URL" ]]; then
    echo "ERROR: No --url / \$OSNET_MODEL_URL provided and no usable existing model at $TARGET." >&2
    echo "See the header of this script for where to obtain the OSNet x1.0 ONNX." >&2
    exit 1
fi

mkdir -p "$MODELS_DIR"

# Atomic move: download to temp + rename, so a Ctrl+C / network drop
# never leaves a half-downloaded file at $TARGET.
TMP=$(mktemp -t "osnet_x1_0.XXXXXXXX.onnx.part")
trap 'rm -f "$TMP"' EXIT

echo "[osnet-deploy] Downloading from $URL"
echo "[osnet-deploy] Target: $TARGET"

if command -v curl &>/dev/null; then
    curl -fSL --retry 3 --retry-delay 2 --connect-timeout 15 -o "$TMP" "$URL"
elif command -v wget &>/dev/null; then
    wget --tries=3 --waitretry=2 --timeout=30 -O "$TMP" "$URL"
else
    echo "ERROR: neither curl nor wget found on PATH." >&2
    exit 1
fi

# ── Sanity-check size ────────────────────────────────────────────────────
# OSNet x1.0 ONNX is consistently ~10 MB. Reject anything wildly off.
size_bytes=$(wc -c < "$TMP" | tr -d ' ')
size_mb=$(( size_bytes / 1048576 ))
if [[ $size_bytes -lt 1048576 ]]; then
    echo "ERROR: Downloaded file is ${size_bytes} bytes (< 1 MB). Likely an HTML error page or wrong URL. Aborting." >&2
    exit 1
fi
if [[ $size_bytes -gt 104857600 ]]; then
    echo "ERROR: Downloaded file is ${size_mb} MB (> 100 MB). OSNet x1.0 is ~10 MB; this is probably the wrong model. Aborting." >&2
    exit 1
fi
echo "[osnet-deploy] Downloaded ${size_mb} MB"

# ── Sanity-check format ──────────────────────────────────────────────────
# Reject obvious HTML/XML error pages. We don't fully parse the protobuf;
# this just catches the most common "got an error page" failure mode.
head_bytes=$(head -c 64 "$TMP" || true)
if [[ "$head_bytes" =~ ^[[:space:]]*\<!DOCTYPE ]] || [[ "$head_bytes" =~ ^[[:space:]]*\<html ]] || [[ "$head_bytes" =~ ^[[:space:]]*\<\?xml ]]; then
    echo "ERROR: Downloaded file looks like HTML/XML, not a binary model." >&2
    echo "First 64 bytes: $head_bytes" >&2
    exit 1
fi

# ── Optional sha256 verification ─────────────────────────────────────────
if [[ -n "$SHA256" ]]; then
    actual=$(sha256_of "$TMP")
    expected="${SHA256,,}"
    if [[ "$actual" != "$expected" ]]; then
        echo "ERROR: sha256 mismatch: got $actual, expected $expected. Aborting (file NOT installed)." >&2
        exit 1
    fi
    echo "[osnet-deploy] sha256 verified: $actual"
else
    echo "[osnet-deploy] No --sha256 given; skipping integrity check (recommended for production)."
fi

# ── Atomic move into place ──────────────────────────────────────────────
mv -f "$TMP" "$TARGET"
trap - EXIT
echo "[osnet-deploy] Installed at $TARGET"
echo "[osnet-deploy] Restart vms_backend for ReIDEngine to pick it up."
