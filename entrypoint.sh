#!/usr/bin/env bash
set -euo pipefail

/app/bin/logana-engine &
ENGINE_PID=$!

cd /app/web
node ./server.js

wait "${ENGINE_PID}"
