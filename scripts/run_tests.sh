#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build_loader.sh" "$ROOT/build"
python3 "$ROOT/src/tools/pack_bootimg.py" "$ROOT/build/mtkloader.bin" "$ROOT/build/begonia-diag.img"
python3 -m unittest discover -s "$ROOT/tests" -v
