#!/usr/bin/env bash
# One-command deploy of the recbean server to a Docker host over SSH.
#
#   ./deploy.sh              package, upload, keep previous image as recbean:prev, rebuild, health check
#   ./deploy.sh --rollback   switch back to recbean:prev (no rebuild, data untouched)
#   ./deploy.sh --status     show container status and health
#
# Environment overrides:
#   DEPLOY_HOST   ssh target              (default root@192.168.2.144)
#   DEPLOY_DIR    remote directory        (default /root/recbean)
#   DEPLOY_PORT   host port of the service (default 8090, must match docker-compose.yml)
#   SSH / SCP / TAR  client binaries      (default ssh / scp / tar from PATH)
#
# Requires key-based SSH to DEPLOY_HOST and Docker Compose v2 there. The remote
# .env (Doubao credentials) and data/ (recordings) are never uploaded or overwritten;
# a missing .env is created from .env.example and must then be filled in on the host.
set -euo pipefail

DEPLOY_HOST="${DEPLOY_HOST:-root@192.168.2.144}"
DEPLOY_DIR="${DEPLOY_DIR:-/root/recbean}"
DEPLOY_PORT="${DEPLOY_PORT:-8090}"
SSH="${SSH:-ssh}"
SCP="${SCP:-scp}"
TAR="${TAR:-tar}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10)

cd "$(dirname "$0")"

remote() {
    "$SSH" "${SSH_OPTS[@]}" "$DEPLOY_HOST" "$@"
}

health() {
    # Poll the health endpoint for up to 30 s; prints the JSON on success.
    remote "for i in \$(seq 1 30); do
              if out=\$(curl -fsS http://localhost:$DEPLOY_PORT/api/health 2>/dev/null); then echo \"\$out\"; exit 0; fi
              sleep 1
            done
            echo 'health check failed' >&2
            cd '$DEPLOY_DIR' && docker compose logs --tail=40 >&2
            exit 1"
}

status() {
    remote "cd '$DEPLOY_DIR' && docker compose ps && docker image ls recbean"
    health
}

rollback() {
    echo "==> rolling back $DEPLOY_HOST:$DEPLOY_DIR to recbean:prev"
    remote "set -e
            docker image inspect recbean:prev >/dev/null 2>&1 || { echo 'no recbean:prev image to roll back to' >&2; exit 1; }
            docker image tag recbean:prev recbean:latest
            cd '$DEPLOY_DIR' && docker compose up -d --no-build"
    health
}

deploy() {
    # Global, not local: the EXIT trap runs after this function has returned.
    TMPDIR_DEPLOY="$(mktemp -d)"
    trap 'rm -rf "$TMPDIR_DEPLOY"' EXIT
    local archive="$TMPDIR_DEPLOY/recbean-deploy.tgz"

    echo "==> packaging server/ (commit $(git rev-parse --short HEAD 2>/dev/null || echo unknown))"
    "$TAR" --exclude=.venv --exclude=node_modules --exclude=dist --exclude=.pytest_cache \
        --exclude=__pycache__ --exclude=.localdata \
        -czf "$archive" docker-compose.yml .env.example server

    echo "==> uploading to $DEPLOY_HOST:$DEPLOY_DIR"
    remote "mkdir -p '$DEPLOY_DIR'"
    "$SCP" "${SSH_OPTS[@]}" -q "$archive" "$DEPLOY_HOST:$DEPLOY_DIR/.deploy.tgz"

    echo "==> rebuilding on $DEPLOY_HOST"
    remote "set -e
            cd '$DEPLOY_DIR'
            if docker image inspect recbean:latest >/dev/null 2>&1; then
                docker image tag recbean:latest recbean:prev
                echo 'kept previous image as recbean:prev'
            fi
            rm -rf server
            # --no-same-owner: archives built on Windows carry uids root cannot chown to.
            tar --no-same-owner -xzf .deploy.tgz
            rm -f .deploy.tgz
            if [ ! -f .env ]; then
                cp .env.example .env && chmod 600 .env
                echo 'WARNING: created .env from .env.example - set DOUBAO_API_KEY in $DEPLOY_DIR/.env' >&2
            fi
            mkdir -p data
            docker compose up -d --build"

    echo "==> health check"
    health
    echo "==> done: http://${DEPLOY_HOST#*@}:$DEPLOY_PORT"
}

case "${1:-}" in
    "") deploy ;;
    --rollback) rollback ;;
    --status) status ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0" ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
esac
