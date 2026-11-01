#!/usr/bin/env bash
# tools/swarm/provision-hermes-role.sh — configure one swarm VM's Hermes for the
# swarm's single model (gaming.md §3.1/§11.3, policy of 2026-10-31).
#
#   ./tools/swarm/provision-hermes-role.sh          # every role: same endpoint, same model
#   ROLE=llm ./tools/swarm/provision-hermes-role.sh
#
# ONE endpoint, ONE model: DeepSeek V4.1 Flash @ http://llm.localdomain:8000/v1 (provider `llm`).
# The old FLASH/DEEP split (provider `dgx` -> dgx1.localdomain) is retired: this script removes
# any `dgx`/`flash` provider blocks it finds so a lane cannot fall back to them. `deep` remains a
# work-class label in gaming.md, not a model.
#
# ROLE=flash|deep are still accepted as legacy aliases of ROLE=llm so old automation keeps working.
#
# Uses `hermes config set/unset` only — never hand-edits config.yaml (Hermes hard invariant).
# Secrets (FORGEJO_TOKEN) go to ~/.hermes/.env, never into a tracked script (gaming.md §11.1).
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"   # non-interactive ssh has a bare PATH; hermes lives here
ROLE="${ROLE:-llm}"
case "$ROLE" in
  llm|flash|deep) NAME=llm; URL=http://llm.localdomain:8000/v1; MODEL=deepseek-v4.1-flash ;;
  *) echo "ROLE must be llm (or legacy flash|deep)" >&2; exit 2 ;;
esac
export ROLE

echo "== $ROLE: provider $NAME -> $URL ($MODEL)"
hermes config set "providers.$NAME.api" "$URL"
hermes config set "providers.$NAME.name" "$NAME"
hermes config set "providers.$NAME.api_key" "not-needed"
hermes config set "providers.$NAME.request_timeout_seconds" 3600
hermes config set "providers.$NAME.stale_timeout_seconds" 2400
hermes config set "providers.$NAME.models" "[\"$MODEL\",\"deepseek-v4-flash\"]"
hermes config set model.default "$MODEL"
hermes config set model.provider "$NAME"
hermes config set model.base_url "$URL"
hermes config set model.api_key "not-needed"

# Retired tiers: a leftover provider block is a fallback path, so remove it.
hermes config unset providers.dgx  >/dev/null 2>&1 || true
hermes config unset providers.flash >/dev/null 2>&1 || true

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
