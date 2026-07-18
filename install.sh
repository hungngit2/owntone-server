#!/bin/bash
# One-click migration from the Docker-based OwnTone deployment to a native
# arm64 .deb install. Run as root on the target Armbian host.
#
# Safe to re-run: re-running after a successful install just upgrades in place.
#
# Rollback: see the "ROLLBACK" comment block at the end of this file.
set -euo pipefail

GITHUB_REPO="${OWNTONE_FORK_REPO:-owntone/owntone-server}"   # override via env if this is a fork
PINNED_VERSION="${OWNTONE_INSTALL_VERSION:-}"                # set to pin, default = latest release
EXISTING_CONF="${OWNTONE_EXISTING_CONF:-/opt/docker/owntone/config/owntone.conf}"
DOCKER_COMPOSE_DIR="${OWNTONE_DOCKER_COMPOSE_DIR:-/opt/docker/owntone}"
DOCKER_CONTAINER_NAME="${OWNTONE_DOCKER_CONTAINER:-owntone}"
OWNTONE_API_PORT="${OWNTONE_API_PORT:-3689}"

log() { echo "[install.sh] $*"; }
die() { echo "[install.sh] ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must be run as root"

ARCH="$(dpkg --print-architecture)"
[ "$ARCH" = "arm64" ] || die "this package is arm64-only, detected '$ARCH'"

log "Resolving release asset..."
if [ -n "$PINNED_VERSION" ]; then
  ASSET_URL="https://github.com/${GITHUB_REPO}/releases/download/v${PINNED_VERSION}/owntone_${PINNED_VERSION}_arm64.deb"
elif command -v jq >/dev/null 2>&1; then
  ASSET_URL="$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
    | jq -r '.assets[].browser_download_url | select(endswith("arm64.deb"))' | head -1)"
  [ -n "$ASSET_URL" ] || die "could not find a arm64.deb asset on the latest release"
else
  ASSET_URL="$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
    | grep -o '"browser_download_url": *"[^"]*arm64\.deb"' \
    | head -1 | sed -E 's/.*"(https[^"]+)"/\1/')"
  [ -n "$ASSET_URL" ] || die "could not find a arm64.deb asset on the latest release"
fi
log "Using asset: $ASSET_URL"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
curl -fsSL -o "$TMPDIR/owntone.deb" "$ASSET_URL"

# --- Safely stop the existing Docker deployment (only if it's running) -----
if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$DOCKER_CONTAINER_NAME"; then
  log "Existing Docker container '$DOCKER_CONTAINER_NAME' is running — checking player state before stopping it"

  PLAYER_STATE="$(curl -fsS "http://127.0.0.1:${OWNTONE_API_PORT}/api/player" 2>/dev/null \
    | grep -o '"state" *: *"[a-z]*"' | sed -E 's/.*"([a-z]+)"$/\1/' || echo "unknown")"

  if [ "$PLAYER_STATE" = "play" ] && [ "${FORCE_STOP:-0}" != "1" ]; then
    die "OwnTone is currently playing (state=play) — refusing to stop the Docker container automatically. Pause playback (or re-run with FORCE_STOP=1) and try again."
  fi
  if [ "$PLAYER_STATE" = "play" ]; then
    log "WARNING: FORCE_STOP=1 set — proceeding despite playback in progress"
  else
    log "Player state is '$PLAYER_STATE' — safe to stop"
  fi

  log "Preserving config: copying $EXISTING_CONF to /etc/owntone.conf before the native package installs its own default"
  mkdir -p /etc
  if [ -f "$EXISTING_CONF" ]; then
    cp -a "$EXISTING_CONF" /etc/owntone.conf.from-docker
  else
    log "WARNING: expected existing config at $EXISTING_CONF not found — nothing to preserve, native install will use its own default"
  fi

  log "Stopping and disabling the Docker container (compose project at $DOCKER_COMPOSE_DIR)"
  if [ -f "$DOCKER_COMPOSE_DIR/docker-compose.yml" ]; then
    (cd "$DOCKER_COMPOSE_DIR" && docker compose down)
  else
    docker stop "$DOCKER_CONTAINER_NAME"
  fi
  docker update --restart=no "$DOCKER_CONTAINER_NAME" 2>/dev/null || true
else
  log "No running '$DOCKER_CONTAINER_NAME' container found — skipping Docker teardown"
  if [ -f "$EXISTING_CONF" ] && [ ! -f /etc/owntone.conf.from-docker ]; then
    cp -a "$EXISTING_CONF" /etc/owntone.conf.from-docker
  fi
fi

# --- Install (or upgrade) the native package --------------------------------
if systemctl is-active --quiet owntone.service 2>/dev/null; then
  log "Native owntone.service already running (upgrade path) — stopping it for the upgrade"
  systemctl stop owntone.service
fi

log "Installing package..."
dpkg -i "$TMPDIR/owntone.deb" || apt-get install -yf   # -yf pulls in missing deps, then retries the pending dpkg config

# --- Adopt the preserved config, if we have one that differs from default --
if [ -f /etc/owntone.conf.from-docker ]; then
  log "Adopting the preserved Docker config as the active owntone.conf"
  cp -a /etc/owntone.conf.from-docker /etc/owntone.conf
fi

log "Enabling and starting owntone.service"
systemctl enable owntone.service
systemctl restart owntone.service

sleep 2
if systemctl is-active --quiet owntone.service; then
  log "owntone.service is running natively. Done."
else
  die "owntone.service failed to start — check 'journalctl -u owntone -e' before retrying"
fi

# ---------------------------------------------------------------------------
# ROLLBACK: if the native install has a problem, go back to the Docker image:
#   systemctl stop owntone.service
#   systemctl disable owntone.service
#   cd /opt/docker/owntone && docker compose up -d
# Your original config is untouched at /opt/docker/owntone/config/owntone.conf
# (this script only ever copies from it, never modifies or deletes it), and a
# copy of what was adopted natively is kept at /etc/owntone.conf.from-docker.
# ---------------------------------------------------------------------------
