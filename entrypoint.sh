#!/usr/bin/env bash
set -euo pipefail

/app/bin/logana-engine &
ENGINE_PID=$!

# Wait for engine HTTP socket to be ready
for i in {1..60}; do
  if bash -c "exec 3<> /dev/tcp/127.0.0.1/24445" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$ENGINE_PID" 2>/dev/null; then
    echo "[FATAL] logana-engine exited unexpectedly before binding to port 24445"
    exit 1
  fi
  sleep 0.5
done

# Final sanity check — if we timed out, abort
if ! bash -c "exec 3<> /dev/tcp/127.0.0.1/24445" 2>/dev/null; then
  echo "[FATAL] logana-engine did not become ready within 30 s"
  kill -TERM "$ENGINE_PID" 2>/dev/null || true
  exit 1
fi

cd /app/web
exec node ./server.js
