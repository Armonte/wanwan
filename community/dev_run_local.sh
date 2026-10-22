#!/usr/bin/env bash
# LOCAL DEV MODE: run the community service on loopback with a fake Discord and THROWAWAY test roots.
#
#   ./dev_run_local.sh                 # fresh data dir, port 8790, removed on exit
#   ./dev_run_local.sh --port 8791
#   ./dev_run_local.sh --keep          # keep the data dir (accounts, test roots) after exit
#   ./dev_run_local.sh --data DIR      # reuse a kept data dir (same test roots, same accounts)
#
# Env: COMMUNITY_DEV_BASE (default ~/.cache/pc-community-dev), COMMUNITY_VENV (default ~/.cache/pc-community-venv),
# COMMUNITY_REPLAY_LOCAL_DIR (default <data dir>/replays: shared replays stored there, served at /fake/replays/).
# The launcher skips its Discord sign-in and sends PC_COMMUNITY_FAKE_DISCORD_TOKEN=fake:<id>[:<name>] instead.
# Never reads or writes the real root keys: the test roots are generated here, in the data dir.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE="${COMMUNITY_DEV_BASE:-$HOME/.cache/pc-community-dev}"
VENV="${COMMUNITY_VENV:-$HOME/.cache/pc-community-venv}"
PY="$VENV/bin/python"
PORT=8790
KEEP=0
DATA=""
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --data) DATA="$2"; KEEP=1; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$BASE" in /mnt/*|[A-Za-z]:*) echo "COMMUNITY_DEV_BASE must be on the Linux filesystem, not $BASE" >&2; exit 2 ;; esac
[ -x "$PY" ] || { echo "no venv at $VENV (python3 -m venv $VENV && $VENV/bin/pip install -r $HERE/requirements.txt)" >&2; exit 2; }

mkdir -p "$BASE"
if [ -z "$DATA" ]; then
    DATA="$(mktemp -d "$BASE/run.XXXXXX")"
fi
if [ "$KEEP" = 0 ]; then
    trap 'rm -rf "$DATA"' EXIT
fi

export COMMUNITY_CONFIG="$DATA/no-such.env"   # never pick up community/data/community.env
export COMMUNITY_DB="$DATA/community.sqlite3"
export COMMUNITY_TICKET_KEY="$DATA/ticket_key.txt"
export COMMUNITY_KEYSET_DIR="$DATA/keysets"
export COMMUNITY_EXPORT_DIR="$DATA/export"      # results.jsonl / hidden.json of test matches stay in the data dir
export COMMUNITY_PUBLIC_HOST=127.0.0.1         # the client signs the host of community.url without its port
export COMMUNITY_ROOT_PATH=""                  # ...and the path after it: no prefix in front of /v1/...
export COMMUNITY_BIND_HOST=127.0.0.1
export COMMUNITY_FAKE_DISCORD=1
export COMMUNITY_DISCORD_CLIENT_ID=""
# shared replays go to a directory in the data dir, served back at /fake/replays/<key> (no R2 in dev mode)
export COMMUNITY_REPLAY_LOCAL_DIR="${COMMUNITY_REPLAY_LOCAL_DIR:-$DATA/replays}"
export COMMUNITY_REPLAY_PUBLIC_BASE="http://127.0.0.1:$PORT/fake/replays"

cd "$HERE"
if [ ! -f "$DATA/test_root_a.seed" ]; then
    echo "== generating throwaway TEST roots, ticket key gen 1 and KEYSET serial 1 in $DATA"
    "$PY" - "$DATA" <<'EOF'
import os, secrets, sys
import tickets as T
d = sys.argv[1]
for n in ("a", "b"):
    p = os.path.join(d, f"test_root_{n}.seed")
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    seed = secrets.token_bytes(32)
    with os.fdopen(fd, "w") as f:
        f.write(seed.hex())
    with open(os.path.join(d, f"test_root_{n}.pub"), "w") as f:
        f.write(T.seed_public(seed).hex())
EOF
fi
ROOT_A="$(cat "$DATA/test_root_a.pub")"
ROOT_B="$(cat "$DATA/test_root_b.pub")"
export COMMUNITY_ROOT_PUBKEYS="$ROOT_A,$ROOT_B"
if [ ! -f "$COMMUNITY_TICKET_KEY" ]; then
    "$PY" admin.py gen-ticket-key --gen 1 >/dev/null
    TPUB="$(sed -n 's/^public=//p' "$COMMUNITY_TICKET_KEY")"
    "$PY" admin.py keyset-body --serial 1 --root a --ticket-key "1:$TPUB" --out "$DATA/keyset_1.body" >/dev/null
    "$PY" sign_keyset.py --roots "$ROOT_A,$ROOT_B" "$DATA/keyset_1.body" "$DATA/test_root_a.seed" - \
        "$DATA/keyset_1.pckeys" >/dev/null
    "$PY" admin.py install-keyset "$DATA/keyset_1.pckeys"
fi

cat <<EOF

== LOCAL DEV community service (FAKE Discord, TEST roots: real clients refuse this KEYSET)
   base URL (launcher ini community.url):  http://127.0.0.1:$PORT
   TEST root A public key:                 $ROOT_A
   TEST root B public key:                 $ROOT_B
   shared replays (local store):           $COMMUNITY_REPLAY_LOCAL_DIR -> $COMMUNITY_REPLAY_PUBLIC_BASE
   data dir:                               $DATA $([ "$KEEP" = 1 ] && echo "(kept)" || echo "(removed on exit)")
   launcher env (instead of Discord):      PC_COMMUNITY_FAKE_DISCORD_TOKEN=fake:4242:Tester
   launcher env (trust the test roots):    PC_COMMUNITY_TEST_ROOTS=$ROOT_A,$ROOT_B

EOF
"$PY" -m uvicorn app:app --host "$COMMUNITY_BIND_HOST" --port "$PORT"
