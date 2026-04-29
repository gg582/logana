#!/usr/bin/env bash
set -euo pipefail

/app/bin/logana-engine &
ENGINE_PID=$!

cd /app/web
./node_modules/.bin/next start --hostname 0.0.0.0 --port 23345

wait "${ENGINE_PID}"
