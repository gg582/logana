#!/usr/bin/env bash
set -uo pipefail

/app/bin/logana-engine &
ENGINE_PID=$!

cd /app/web
exec node ./server.js
