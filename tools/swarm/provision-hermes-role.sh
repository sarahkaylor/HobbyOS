#!/usr/bin/env bash
# tools/swarm/provision-hermes-role.sh — configure one swarm VM's Hermes for its
# model tier (GB-004, gaming.md §3.1/§11.3).
#
#   ROLE=flash  ./tools/swarm/provision-hermes-role.sh   # worker/reviewer/tester VMs (farms)
#   ROLE=deep   ./tools/swarm/provision-hermes-role.sh   # swarm-coordinator only
#
# FLASH = DeepSeek V4 Flash   @ http://llm.localdomain:8000/v1   (RTX-6000 pair box)
# DEEP  = DeepSeek v4.1 Flash @ http://dgx1.localdomain:8000/v1  (8x DGX Spark fleet)
#
# Uses `hermes config set` only — never hand-edits config.yaml (Hermes hard invariant).
# Secrets (FORGEJO_TOKEN) go to ~/.hermes/.env, never into a tracked script (gaming.md §11.1).
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"   # non-interactive ssh has a bare PATH; hermes lives here
ROLE="${ROLE:-flash}"
case "$ROLE" in
  flash) NAME=flash; URL=http://llm.localdomain:8000/v1;   MODEL=deepseek-v4-flash ;;
  deep)  NAME=dgx;   URL=http://dgx1.localdomain:8000/v1;  MODEL=deepseek-v4.1-flash ;;
  *) echo "ROLE must be flash|deep" >&2; exit 2 ;;
esac
export ROLE

echo "== $ROLE: provider $NAME -> $URL ($MODEL)"
hermes config set "providers.$NAME.api" "$URL"
hermes config set "providers.$NAME.name" "$NAME"
hermes config set "providers.$NAME.api_key" "not-needed"
hermes config set "providers.$NAME.request_timeout_seconds" 3600
hermes config set "providers.$NAME.stale_timeout_seconds" 2400
hermes config set "providers.$NAME.models" "[\"$MODEL\"]"
hermes config set model.default "$MODEL"
hermes config set model.provider "$NAME"
hermes config set model.base_url "$URL"
hermes config set model.api_key "not-needed"

# board access for the agent (secret -> .env, per §11.1)
ENVF="$HOME/.hermes/.env"
touch "$ENVF"; chmod 600 "$ENVF"
grep -q '^FORGEJO_URL=' "$ENVF" || echo "FORGEJO_URL=${FORGEJO_URL:-http://192.168.0.234:3000}" >> "$ENVF"
if [ -n "${FORGEJO_TOKEN:-}" ] && ! grep -q '^FORGEJO_TOKEN=' "$ENVF"; then
  echo "FORGEJO_TOKEN=$FORGEJO_TOKEN" >> "$ENVF"
fi

echo "== resolved:"
hermes config get model | head -4
echo "== probe:"
curl -s -m 10 "$URL/models" | python3 -c "import sys,json;print('  endpoint models:',[m['id'] for m in json.load(sys.stdin)['data']])"
echo "OK $ROLE"
