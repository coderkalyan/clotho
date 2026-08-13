#!/usr/bin/env bash
# In-box cost safety: delete THIS Latitude server when it goes idle (local mode)
# or when it exceeds a hard lifetime ceiling (backstop if the local orchestrator
# dies before it can `terraform destroy`). Powering off does NOT stop hourly
# billing — only deletion does — so this issues the API delete.
#
# Armed by run.sh over SSH (writes watchdog.env + latitude.token), fired by a
# systemd timer every ~2 min. Set CLOTHO_WATCHDOG_DRYRUN=1 to log instead of
# deleting (used by the offline self-test).
set -euo pipefail

CONF=${CLOTHO_WATCHDOG_CONF:-/opt/clotho/watchdog.env}
TOKEN_FILE=${CLOTHO_WATCHDOG_TOKEN:-/opt/clotho/latitude.token}
HEARTBEAT=${CLOTHO_WATCHDOG_HEARTBEAT:-/opt/clotho/heartbeat}
LOG() { echo "[watchdog $(date -u +%FT%TZ)] $*"; }

[ -f "$CONF" ] || { LOG "no config at $CONF — not armed, exiting"; exit 0; }
# shellcheck disable=SC1090
. "$CONF"    # SERVER_ID, IDLE_MINUTES, MAX_MINUTES, ARM_EPOCH, API_BASE, AUTH_HEADER

: "${SERVER_ID:?}"; : "${API_BASE:=https://api.latitude.sh}"
: "${IDLE_MINUTES:=60}"; : "${MAX_MINUTES:=180}"; : "${ARM_EPOCH:=0}"
: "${AUTH_HEADER:=Authorization: Bearer __TOKEN__}"   # verify scheme at signup

now=$(date +%s)

# Activity = an interactive SSH session OR a running benchmark. Either refreshes
# the heartbeat; the idle clock is time since the last such refresh.
active=0
who 2>/dev/null | grep -q . && active=1
pgrep -f 'perf\.py|clotho-(base|head)|bench-remote' >/dev/null 2>&1 && active=1
if [ "$active" = 1 ]; then
  touch "$HEARTBEAT"
  LOG "active (ssh/bench) — heartbeat refreshed"
fi
[ -f "$HEARTBEAT" ] || touch "$HEARTBEAT"

idle_for=$(( now - $(stat -c %Y "$HEARTBEAT") ))
life_for=$(( now - ARM_EPOCH ))

reason=""
if [ "$MAX_MINUTES" -gt 0 ] && [ "$life_for" -ge $(( MAX_MINUTES * 60 )) ]; then
  reason="hard cap: lifetime ${life_for}s >= ${MAX_MINUTES}m"
elif [ "$IDLE_MINUTES" -gt 0 ] && [ "$idle_for" -ge $(( IDLE_MINUTES * 60 )) ]; then
  reason="idle: ${idle_for}s >= ${IDLE_MINUTES}m with no ssh/bench"
fi

if [ -z "$reason" ]; then
  LOG "keep: idle ${idle_for}s (limit $((IDLE_MINUTES*60))s), life ${life_for}s (cap $((MAX_MINUTES*60))s)"
  exit 0
fi

LOG "DELETE server $SERVER_ID — $reason"
auth=${AUTH_HEADER/__TOKEN__/$(cat "$TOKEN_FILE" 2>/dev/null || true)}
if [ "${CLOTHO_WATCHDOG_DRYRUN:-0}" = 1 ]; then
  LOG "DRYRUN: curl -sf -X DELETE -H '<auth>' $API_BASE/servers/$SERVER_ID"
  exit 0
fi
# Retry a few times; a failed delete keeps billing, so it is worth insisting.
for i in 1 2 3 4 5; do
  if curl -sf -X DELETE -H "$auth" "$API_BASE/servers/$SERVER_ID"; then
    LOG "delete request accepted"; exit 0
  fi
  LOG "delete attempt $i failed; retrying"; sleep 10
done
LOG "ERROR: could not delete server after retries — CHECK DASHBOARD, still billing"
exit 1
