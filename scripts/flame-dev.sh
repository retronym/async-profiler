#!/usr/bin/env bash
# Viewer dev loop: rebuild jfrconv, render each seed JFR (given as name=path, or from $FLAME_SEEDS) into build/dev/<name>.html,
# and serve build/dev on http://127.0.0.1:${PORT:-8791}/ (once). Edit src/res/flame.html, rerun, reload the page.
set -euo pipefail
cd "$(dirname "$0")/.."
make build/bin/jfrconv >/dev/null
mkdir -p build/dev
for seed in "$@"; do
  name=${seed%%=*}; jfr=${seed#*=}
  build/bin/jfrconv --hw cycles --threads --classify --minwidth 0.001 --display-minwidth 0.01 "$jfr" "build/dev/$name.html" >/dev/null
done
ls build/dev/*.html | sed 's|^|rendered |'
if ! lsof -iTCP:"${PORT:-8791}" -sTCP:LISTEN >/dev/null 2>&1; then
  (cd build/dev && nohup python3 -m http.server "${PORT:-8791}" --bind 127.0.0.1 >/dev/null 2>&1 &)
  sleep 1
fi
echo "http://127.0.0.1:${PORT:-8791}/"
