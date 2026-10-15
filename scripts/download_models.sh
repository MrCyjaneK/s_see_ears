#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
MODELS="models"
mkdir -p "$MODELS"

fetch() {
  local name="$1" url="$2"
  if [[ -f "$MODELS/$name" && -s "$MODELS/$name" ]]; then
    echo "[=] $name"
    return 0
  fi
  curl -fSL -L --progress-bar "$url" -o "$MODELS/$name.part"
  mv "$MODELS/$name.part" "$MODELS/$name"
}

fetch ggml-medium.bin \
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin"

python3 scripts/gen_assets.py "$MODELS" src/assets.cpp
