#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
MEDIA="testdata/media"
mkdir -p "$MEDIA"

SBS_PREFIX="${SBS_PREFIX:-$HOME/sbs/_/x86_64-linux-gnu}"

speech_wav="$MEDIA/speech.wav"
video_mp4="$MEDIA/speech.mp4"

fetch() {
  local name="$1" url="$2"
  local dest="$MEDIA/$name"
  if [[ -f "$dest" && -s "$dest" ]]; then
    echo "[=] $name"
    return 0
  fi
  echo "[+] downloading $name"
  curl -fsSL -L "$url" -o "$dest"
}

if [[ ! -f "$speech_wav" || ! -s "$speech_wav" ]]; then
  echo "[+] downloading short speech sample"
  curl -fsSL -L \
    "https://upload.wikimedia.org/wikipedia/commons/1/1f/En-us-hello.ogg" \
    -o "$MEDIA/hello.ogg"
  ffmpeg -y -i "$MEDIA/hello.ogg" -ar 16000 -ac 1 "$speech_wav"
  rm -f "$MEDIA/hello.ogg"
fi

if [[ ! -f "$video_mp4" || ! -s "$video_mp4" ]]; then
  echo "[+] generating speech.mp4 from speech.wav"
  ffmpeg -y \
    -f lavfi -i "color=c=black:s=320x240:d=3" \
    -i "$speech_wav" \
    -c:v libx264 -tune stillimage -pix_fmt yuv420p \
    -c:a aac -shortest \
    "$video_mp4"
fi

fetch jfk.wav "https://github.com/ggml-org/whisper.cpp/raw/master/samples/jfk.wav"