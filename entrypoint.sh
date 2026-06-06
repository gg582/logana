#!/usr/bin/env bash
set -uo pipefail

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_legend=1

# Restart engine automatically if it crashes (watchdog loop)
while true; do
  /app/bin/logana-engine &
  ENGINE_PID=$!

  # Wait for engine HTTP socket to be ready
  for i in {1..60}; do
    if bash -c "exec 3<> /dev/tcp/127.0.0.1/24445" 2>/dev/null; then
      break
    fi
    if ! kill -0 "$ENGINE_PID" 2>/dev/null; then
      echo "[WARN] logana-engine exited unexpectedly before binding to port 24445 — restarting in 1 s"
      sleep 1
      continue 2
    fi
    sleep 0.5
  done

  if ! bash -c "exec 3<> /dev/tcp/127.0.0.1/24445" 2>/dev/null; then
    echo "[WARN] logana-engine did not become ready within 30 s — restarting"
    kill -TERM "$ENGINE_PID" 2>/dev/null || true
    sleep 1
    continue
  fi

  echo "[INFO] logana-engine ready (pid $ENGINE_PID)"

  # Wait until engine dies; then restart
  while kill -0 "$ENGINE_PID" 2>/dev/null; do
    sleep 1
  done
  echo "[WARN] logana-engine died — restarting in 1 s"
  sleep 1
done &

cd /app/web
exec node ./server.js
