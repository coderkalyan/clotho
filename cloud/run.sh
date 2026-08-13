#!/usr/bin/env bash
# One-shot differential benchmark on a fresh Latitude.sh box.
#
#   ci mode:    provision -> bench -> destroy (unattended, e.g. GitHub Actions)
#   local mode: provision -> bench -> LEAVE UP; the in-box watchdog self-deletes
#               after --idle-min of no ssh/bench (keep it warm while you iterate).
# Either way a hard cap (--max-min) and an on-error `terraform destroy` trap are
# the cost backstops. Deletion — not power-off — is what stops hourly billing.
#
#   export LATITUDESH_AUTH_TOKEN=...        # terraform + watchdog auth
#   cloud/run.sh --base main --head my-feature --mode local
#   cloud/run.sh --base main --head my-feature --mode ci --cases picorv32 gate_rca
set -euo pipefail

REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
CLOUD="$REPO/cloud"; TF="$CLOUD/terraform"
base= head= mode=local runs=5 idle_min=60 max_min=180 oracles=0 rebuild=0
ssh_user=root ssh_key=~/.ssh/id_ed25519 auth_header="Authorization: Bearer __TOKEN__"
cases=()
while [ $# -gt 0 ]; do case "$1" in
  --base) base=$2; shift 2;; --head) head=$2; shift 2;;
  --mode) mode=$2; shift 2;; --runs) runs=$2; shift 2;;
  --idle-min) idle_min=$2; shift 2;; --max-min) max_min=$2; shift 2;;
  --ssh-user) ssh_user=$2; shift 2;; --ssh-key) ssh_key=$2; shift 2;;
  --auth-header) auth_header=$2; shift 2;;
  --oracles) oracles=1; shift;; --rebuild) rebuild=1; shift;;
  --cases) shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do cases+=("$1"); shift; done;;
  *) echo "run.sh: unknown arg '$1'" >&2; exit 2;; esac
done
[ -n "$base" ] && [ -n "$head" ] || { echo "usage: run.sh --base REF --head REF [--mode ci|local]" >&2; exit 2; }
[ -n "${LATITUDESH_AUTH_TOKEN:-}" ] || { echo "run.sh: export LATITUDESH_AUTH_TOKEN first" >&2; exit 2; }
WATCHDOG_TOKEN=${WATCHDOG_TOKEN:-$LATITUDESH_AUTH_TOKEN}
[ "$WATCHDOG_TOKEN" = "$LATITUDESH_AUTH_TOKEN" ] && \
  echo "run.sh: WARNING watchdog reuses your main token; a dedicated rotatable token is safer" >&2

# 1. Build the two static binaries locally (free; skipped if present).
if [ "$rebuild" = 1 ] || [ ! -x "$CLOUD/out/clotho-base" ] || [ ! -x "$CLOUD/out/clotho-head" ]; then
  echo "== build-refs (local static) =="
  "$CLOUD/build-refs.sh" --base "$base" --head "$head"
fi

# 2. Provision. Destroy-on-error is the primary cost guard for the paid phase.
echo "== terraform apply =="
terraform -chdir="$TF" init -input=false >/dev/null
destroy() { echo "== terraform destroy =="; terraform -chdir="$TF" destroy -auto-approve || \
  echo "run.sh: DESTROY FAILED — CHECK DASHBOARD, still billing"; }
trap 'rc=$?; [ $rc -ne 0 ] && destroy; exit $rc' ERR
terraform -chdir="$TF" apply -auto-approve -input=false
IP=$(terraform -chdir="$TF" output -raw ipv4)
ID=$(terraform -chdir="$TF" output -raw server_id)
echo "server $ID at $IP"

SSH="ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -i $ssh_key $ssh_user@$IP"
SCP="scp -o StrictHostKeyChecking=accept-new -i $ssh_key"

# 3. Wait for sshd, then for cloud-init (box-setup) to finish.
echo "== wait for ssh + cloud-init =="
for i in $(seq 1 60); do $SSH true 2>/dev/null && break; sleep 5; done
$SSH 'cloud-init status --wait || true; test -f /opt/clotho/.box-setup-done'

# 4. Arm the watchdog BEFORE the bench, so a crash mid-run is still capped.
echo "== arm watchdog (idle ${idle_min}m, cap ${max_min}m) =="
$SSH 'mkdir -p /opt/clotho'
$SCP "$CLOUD/watchdog/clotho-watchdog.sh" "$ssh_user@$IP:/opt/clotho/clotho-watchdog.sh"
$SCP "$CLOUD/watchdog/clotho-watchdog.service" "$ssh_user@$IP:/etc/systemd/system/clotho-watchdog.service"
$SCP "$CLOUD/watchdog/clotho-watchdog.timer" "$ssh_user@$IP:/etc/systemd/system/clotho-watchdog.timer"
# ci mode: idle-kill off (0); rely on the destroy below + hard cap.
idle_eff=$idle_min; [ "$mode" = ci ] && idle_eff=0
printf '%s' "$WATCHDOG_TOKEN" | $SSH 'cat > /opt/clotho/latitude.token && chmod 600 /opt/clotho/latitude.token'
$SSH "cat > /opt/clotho/watchdog.env <<EOF
SERVER_ID=$ID
IDLE_MINUTES=$idle_eff
MAX_MINUTES=$max_min
ARM_EPOCH=\$(date +%s)
API_BASE=https://api.latitude.sh
AUTH_HEADER=$auth_header
EOF
chmod 755 /opt/clotho/clotho-watchdog.sh
systemctl daemon-reload && systemctl enable --now clotho-watchdog.timer"

# 5. Ship binaries + workload + runner.
echo "== ship binaries + tests/perf =="
$SSH 'mkdir -p /opt/clotho/bench'
$SCP "$CLOUD"/out/clotho-base "$CLOUD"/out/clotho-base.sha \
     "$CLOUD"/out/clotho-head "$CLOUD"/out/clotho-head.sha \
     "$CLOUD/bench-remote.sh" "$ssh_user@$IP:/opt/clotho/bench/"
rsync -az -e "ssh -o StrictHostKeyChecking=accept-new -i $ssh_key" \
  "$REPO/tests/perf" "$ssh_user@$IP:/opt/clotho/bench/tests/"

# 6. Run the differential (pinned to an isolated core inside bench-remote).
echo "== bench-remote =="
oracle_flag=""; [ "$oracles" = 1 ] && oracle_flag="--oracles"
case_flag=""; [ ${#cases[@]} -gt 0 ] && case_flag="--cases ${cases[*]}"
set +e
$SSH "chmod +x /opt/clotho/bench/clotho-base /opt/clotho/bench/clotho-head /opt/clotho/bench/bench-remote.sh; \
  cd /opt/clotho/bench && ./bench-remote.sh \
    --base-bin clotho-base --head-bin clotho-head \
    --perf-dir tests/perf --runs $runs $oracle_flag $case_flag --out report"
gate=$?
set -e

# 7. Pull the report bundle back.
stamp=$(date -u +%Y%m%dT%H%M%SZ)
dest="$CLOUD/reports/$stamp"; mkdir -p "$dest"
rsync -az -e "ssh -o StrictHostKeyChecking=accept-new -i $ssh_key" \
  "$ssh_user@$IP:/opt/clotho/bench/report/" "$dest/" || true
echo "== report -> $dest (gate rc=$gate) =="

# 8. Teardown.
trap - ERR
if [ "$mode" = ci ]; then
  destroy
else
  echo "== local mode: server $ID left UP at $IP =="
  echo "   reconnect: ssh -i $ssh_key $ssh_user@$IP"
  echo "   auto-deletes after ${idle_min}m idle (or ${max_min}m hard cap)"
  echo "   delete now: terraform -chdir=$TF destroy -auto-approve"
fi
exit "$gate"
