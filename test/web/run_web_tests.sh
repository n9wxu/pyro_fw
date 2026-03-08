#!/bin/bash
# Run web UI tests against mock server in all 3 modes.
# Usage: ./run_web_tests.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT=3456
PASS=0
FAIL=0

for MODE in new configured flown; do
  echo "══════════════════════════════════════════"
  echo "  Testing mode: $MODE"
  echo "══════════════════════════════════════════"

  # Start mock server
  node "$SCRIPT_DIR/mock_server.js" "$MODE" "$PORT" &
  SERVER_PID=$!
  sleep 1

  # Run playwright tests
  if PYRO_MODE="$MODE" BASE_URL="http://localhost:$PORT" \
     npx playwright test --config="$SCRIPT_DIR/playwright.config.js" 2>&1; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
  fi

  # Stop server
  kill $SERVER_PID 2>/dev/null || true
  wait $SERVER_PID 2>/dev/null || true
done

echo ""
echo "══════════════════════════════════════════"
echo "  Results: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════"

[ $FAIL -eq 0 ]
