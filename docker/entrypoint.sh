#!/usr/bin/env bash
set -e

CONFIG_FILE="/etc/grocycle/config.json"
mkdir -p /etc/grocycle /var/lib/grocycle /run/grocycle /var/log

# Parse scan interval duration string (e.g. 15m, 1h, 24h, 7d) into seconds
RAW_INTERVAL="${GROCYCLE_SCAN_INTERVAL:-3600}"
if [[ "$RAW_INTERVAL" =~ ^([0-9]+)m(in)?$ ]]; then
    INTERVAL=$(( ${BASH_REMATCH[1]} * 60 ))
elif [[ "$RAW_INTERVAL" =~ ^([0-9]+)h(r)?$ ]]; then
    INTERVAL=$(( ${BASH_REMATCH[1]} * 3600 ))
elif [[ "$RAW_INTERVAL" =~ ^([0-9]+)d(ay)?$ ]]; then
    INTERVAL=$(( ${BASH_REMATCH[1]} * 86400 ))
elif [[ "$RAW_INTERVAL" =~ ^[0-9]+$ ]]; then
    INTERVAL="$RAW_INTERVAL"
else
    echo "[grocycle-docker] Warning: unrecognized interval format '$RAW_INTERVAL', defaulting to 3600s (1h)"
    INTERVAL=3600
fi

# Build JSON array for monitored roots from comma-separated list
IFS=',' read -ra ROOTS_ARR <<< "${GROCYCLE_MONITORED_ROOTS:-/monitored}"
ROOTS_JSON="[]"
if command -v jq >/dev/null 2>&1; then
    ROOTS_JSON=$(printf '%s\n' "${ROOTS_ARR[@]}" | jq -R . | jq -s .)
else
    ROOTS_JSON="["
    FIRST=1
    for r in "${ROOTS_ARR[@]}"; do
        r=$(echo "$r" | xargs) # trim whitespace
        if [ -n "$r" ]; then
            if [ $FIRST -eq 0 ]; then ROOTS_JSON+=", "; fi
            ROOTS_JSON+="\"$r\""
            FIRST=0
        fi
    done
    ROOTS_JSON+="]"
fi

# Build JSON array for excluded prefixes from comma-separated list
IFS=',' read -ra EXCLUDES_ARR <<< "${GROCYCLE_EXCLUDED_PREFIXES:-/proc,/sys,/dev,/run,/tmp,/var/tmp}"
EXCLUDES_JSON="[]"
if command -v jq >/dev/null 2>&1; then
    EXCLUDES_JSON=$(printf '%s\n' "${EXCLUDES_ARR[@]}" | jq -R . | jq -s .)
else
    EXCLUDES_JSON="["
    FIRST=1
    for p in "${EXCLUDES_ARR[@]}"; do
        p=$(echo "$p" | xargs)
        if [ -n "$p" ]; then
            if [ $FIRST -eq 0 ]; then EXCLUDES_JSON+=", "; fi
            EXCLUDES_JSON+="\"$p\""
            FIRST=0
        fi
    done
    EXCLUDES_JSON+="]"
fi

# Generate /etc/grocycle/config.json dynamically
cat <<EOF > "$CONFIG_FILE"
{
    "db_path": "${GROCYCLE_DB_PATH:-/var/lib/grocycle/grocycle.db}",
    "socket_path": "${GROCYCLE_SOCKET_PATH:-/run/grocycle/grocycle.sock}",
    "log_file": "${GROCYCLE_LOG_FILE:-/var/log/grocycle.log}",
    "scan_interval_seconds": ${INTERVAL},
    "sleep_every_n_dirs": ${GROCYCLE_THROTTLE_BATCH:-250},
    "sleep_duration_ms": ${GROCYCLE_THROTTLE_SLEEP_MS:-15},
    "monitored_roots": ${ROOTS_JSON},
    "excluded_prefixes": ${EXCLUDES_JSON}
}
EOF

echo "[grocycle-docker] Configuration generated at $CONFIG_FILE:"
cat "$CONFIG_FILE"
echo ""

# Start background snapshot engine (grocycled)
echo "[grocycle-docker] Starting background scanner (grocycled)..."
grocycled -c "$CONFIG_FILE" -f &
DAEMON_PID=$!

# Trap signals for graceful shutdown
cleanup() {
    echo "[grocycle-docker] Shutting down Grocycle services..."
    if [ -n "$WEB_PID" ] && kill -0 "$WEB_PID" 2>/dev/null; then
        kill -TERM "$WEB_PID" 2>/dev/null || true
    fi
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
    fi
    wait "$DAEMON_PID" 2>/dev/null || true
    wait "$WEB_PID" 2>/dev/null || true
    echo "[grocycle-docker] All services stopped cleanly."
    exit 0
}
trap cleanup SIGTERM SIGINT SIGHUP

# Assemble web server command arguments
WEB_ARGS=(
    "-c" "$CONFIG_FILE"
    "-p" "${GROCYCLE_WEB_PORT:-8080}"
    "-H" "0.0.0.0"
    "--max-clients" "${GROCYCLE_WEB_MAX_CLIENTS:-5}"
)

if [ -n "$GROCYCLE_WEB_AUTH" ]; then
    WEB_ARGS+=("-a" "$GROCYCLE_WEB_AUTH")
fi

echo "[grocycle-docker] Starting web UI on port ${GROCYCLE_WEB_PORT:-8080}..."
grocycle web "${WEB_ARGS[@]}" &
WEB_PID=$!

# Wait for either process to terminate
wait -n "$WEB_PID" "$DAEMON_PID"
cleanup
