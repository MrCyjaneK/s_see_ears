#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

./build_linux.sh
./scripts/download_test_media.sh

if [[ ! -d models ]]; then
  echo "[!] models/ missing — run ./scripts/download_models.sh first" >&2
  exit 1
fi

SBS_PREFIX="${SBS_PREFIX:-$HOME/sbs/_/x86_64-linux-gnu}"
export LD_LIBRARY_PATH="$PWD/dist:$SBS_PREFIX/lib:${LD_LIBRARY_PATH:-}"

OUT="/tmp/ears-test-out.json"
for media in testdata/media/*; do
  [[ -f "$media" ]] || continue
  echo "[+] analyzing $media"
  EARS_ASSETS_DIR="$PWD/models" ./dist/test_ears "$media" "$OUT"
  python3 - <<PY
import json, sys
j=json.load(open("$OUT"))
tr=j.get("transcript",{})
segs=tr.get("segments",[])
text="".join(s.get("text","") for s in segs).strip()
print("  language:", tr.get("language"))
print("  segments:", len(segs))
print("  text:", text[:120])
print("  tokens:", sum(len(s.get("tokens",[])) for s in segs))
if segs:
    print("  first segment confidence:", segs[0].get("confidence"))
if "translation" in j:
    sys.exit("unexpected translation field (translation is disabled)")
if not text:
    sys.exit("empty transcript")
PY
done
echo "[+] all tests passed"
