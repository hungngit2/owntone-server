#!/bin/bash
# One-click migration from the Docker-based OwnTone deployment to a native
# arm64 .deb install. Run as root on the target Armbian host:
#   curl -fsSL https://raw.githubusercontent.com/hungngit2/owntone-server/master/install.sh | sudo bash
# (use `bash`, not `sh` -- this script relies on a couple of bash-only
# constructs, so piping into dash/POSIX sh will fail partway through)
#
# Safe to re-run: re-running after a successful install just upgrades in
# place, and only ever adopts the preserved Docker config once (see
# ADOPTED_MARKER below) -- later re-runs never overwrite your live
# /etc/owntone.conf, however you've since tuned it.
#
# Rollback: see the "ROLLBACK" comment block at the end of this file.
set -euo pipefail

# When run as `curl ... | sudo bash`, this script's own stdin is still the
# tail of that network pipe, not a terminal -- and dpkg/postinst/systemctl can
# each independently try to read from stdin (an interactive prompt, or just
# checking whether it's a tty). If they get an unexpected pipe instead, they
# can fail in confusing ways (e.g. "Could not execute systemctl" from
# deb-systemd-invoke) partway through, well after the script itself has
# already started running.
#
# NOTE: this must NOT be done via a top-level `exec < /dev/null` here. When
# bash is reading this very script from a pipe (not a seekable file), it
# reads it incrementally rather than buffering the whole thing up front --
# reassigning the script's own stdin mid-script cuts off everything bash
# hasn't read yet, so the rest of the script silently never runs (confirmed
# live: `bash -x` trace stopped dead right after this line, exit 0, nothing
# after it executed). Instead, redirect stdin only on the specific
# subprocesses below that actually need it detached.

GITHUB_REPO="${OWNTONE_FORK_REPO:-hungngit2/owntone-server}"   # override via env for a different fork/upstream
PINNED_VERSION="${OWNTONE_INSTALL_VERSION:-}"                # set to pin, default = latest release
EXISTING_CONF="${OWNTONE_EXISTING_CONF:-/opt/docker/owntone/config/owntone.conf}"
DOCKER_COMPOSE_DIR="${OWNTONE_DOCKER_COMPOSE_DIR:-/opt/docker/owntone}"
DOCKER_CONTAINER_NAME="${OWNTONE_DOCKER_CONTAINER:-owntone}"
OWNTONE_API_PORT="${OWNTONE_API_PORT:-3689}"

log() { echo "[install.sh] $*"; }
die() { echo "[install.sh] ERROR: $*" >&2; exit 1; }

# Pulls every quoted value out of a libconfuse `key = "..."` or
# `key = { "...", "..." }` assignment (comments and unrelated keys ignored).
# Good enough for the handful of path-like settings we care about here —
# not a general libconfuse parser.
conf_values() {
  local key="$1" file="$2"
  # A key that's absent/commented-out is normal (e.g. db_path defaults to
  # unset), not an error -- don't let pipefail + set -e turn "no match" into
  # a script abort.
  grep -E "^[[:space:]]*${key}[[:space:]]*=" "$file" 2>/dev/null \
    | sed -E "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*//" \
    | grep -o '"[^"]*"' \
    | sed -e 's/^"//' -e 's/"$//' \
    || true
}

# Warns (loudly, non-fatal) if the adopted config's `directories` / `db_path`
# point at paths that don't exist on this host — the classic symptom of
# copying a Docker container's owntone.conf (container-internal paths like
# /srv/music) straight onto the native host without remapping.
check_conf_paths() {
  local conf="$1"
  local missing=0
  local dirs db d p pdir

  dirs="$(conf_values "directories" "$conf")"
  if [ -n "$dirs" ]; then
    while IFS= read -r d; do
      [ -n "$d" ] || continue
      if [ ! -d "$d" ]; then
        log "WARNING: library directory '$d' from the adopted config does not exist on this host"
        missing=1
      fi
    done <<< "$dirs"
  fi

  db="$(conf_values "db_path" "$conf")"
  if [ -n "$db" ]; then
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      pdir="$(dirname "$p")"
      if [ ! -d "$pdir" ]; then
        log "WARNING: db_path '$p' from the adopted config lives in a directory that does not exist on this host"
        missing=1
      fi
    done <<< "$db"
  fi

  if [ "$missing" -eq 1 ]; then
    log "############################################################################"
    log "# WARNING: the adopted /etc/owntone.conf references paths that do not     #"
    log "# exist on this host. This is expected if it was copied from a Docker     #"
    log "# deployment whose 'directories'/'db_path' pointed at container-internal  #"
    log "# paths (e.g. bind-mounted /srv/music) rather than real host paths.       #"
    log "# OwnTone will start against an effectively empty/broken library and may  #"
    log "# trigger a full rescan -- expensive on a memory/swap-constrained host.   #"
    log "# Edit /etc/owntone.conf to point at the correct HOST paths, then run:    #"
    log "#   systemctl restart owntone.service                                    #"
    log "############################################################################"
  fi
}

# A Docker deployment's owntone.conf commonly sets logfile to /dev/stderr (or
# /dev/stdout) so `docker logs` captures it. Under systemd, the service's
# stdout/stderr are journal-connected, not a real device node -- opening
# /dev/stderr there fails with ENXIO and owntone exits immediately, before it
# even reaches the httpd bind step. Rewrite it to a real file unconditionally;
# it's never correct for a systemd-managed native install.
fix_conf_logfile() {
  local conf="$1"
  if grep -qE '^[[:space:]]*logfile[[:space:]]*=[[:space:]]*"/dev/stderr"' "$conf" \
    || grep -qE '^[[:space:]]*logfile[[:space:]]*=[[:space:]]*"/dev/stdout"' "$conf"; then
    log "Adopted config sets logfile to /dev/stderr or /dev/stdout (a Docker-logging convention) -- rewriting to /var/log/owntone.log, since that fails under systemd"
    sed -i 's|logfile = "/dev/stderr"|logfile = "/var/log/owntone.log"|' "$conf"
    sed -i 's|logfile = "/dev/stdout"|logfile = "/var/log/owntone.log"|' "$conf"
  fi
}

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

# --- Check whether the Docker deployment is up, and whether it's safe to ---
# --- touch it -- this must happen before ANY Docker teardown, but does   ---
# --- not itself stop anything yet (see below for why teardown is delayed
# --- until after a successful native package install).
DOCKER_RUNNING=false
if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$DOCKER_CONTAINER_NAME"; then
  DOCKER_RUNNING=true
  log "Existing Docker container '$DOCKER_CONTAINER_NAME' is running — checking player state"

  PLAYER_STATE="$(curl -fsS "http://127.0.0.1:${OWNTONE_API_PORT}/api/player" 2>/dev/null \
    | grep -o '"state" *: *"[a-z]*"' | sed -E 's/.*"([a-z]+)"$/\1/' || echo "unknown")"

  if [ "$PLAYER_STATE" = "play" ] && [ "${FORCE_STOP:-0}" != "1" ]; then
    die "OwnTone is currently playing (state=play) — refusing to touch the Docker container automatically. Pause playback (or re-run with FORCE_STOP=1) and try again."
  fi
  if [ "$PLAYER_STATE" = "play" ]; then
    log "WARNING: FORCE_STOP=1 set — proceeding despite playback in progress"
  else
    log "Player state is '$PLAYER_STATE' — safe to proceed"
  fi
else
  log "No running '$DOCKER_CONTAINER_NAME' container found — skipping Docker player-state check"
fi

# --- Preserve the existing config for later adoption ------------------------
# Read-only against the Docker bind mount; never touches/removes the original.
# Safe to do regardless of whether Docker is currently running.
if [ -f /etc/owntone.conf.from-docker ]; then
  log "Preserved config already present at /etc/owntone.conf.from-docker — leaving it as-is"
elif [ -f "$EXISTING_CONF" ]; then
  mkdir -p /etc
  cp -a "$EXISTING_CONF" /etc/owntone.conf.from-docker
  log "Preserved config: copied $EXISTING_CONF to /etc/owntone.conf.from-docker"
else
  log "WARNING: expected existing config at $EXISTING_CONF not found — nothing to preserve, native install will use its own default"
fi

# --- Install (or upgrade) the native package --------------------------------
if systemctl is-active --quiet owntone.service 2>/dev/null; then
  log "Native owntone.service already running (upgrade path) — stopping it for the upgrade"
  systemctl stop owntone.service < /dev/null
fi

log "Installing package..."
# < /dev/null: postinst's `deb-systemd-invoke` can otherwise inherit this
# script's own stdin (the curl pipe, when piped) and fail confusingly (e.g.
# "Could not execute systemctl") -- see the note on stdin handling above.
dpkg -i "$TMPDIR/owntone.deb" < /dev/null || apt-get install -yf < /dev/null   # -yf pulls in missing deps, then retries the pending dpkg config
log "Package install succeeded"

# postinst may run 'deb-systemd-invoke restart owntone.service' as part of
# configuring the package. If the old Docker container is still up at this
# point, both could momentarily fight over the same port. Stop the
# just-(re)started native service now, before we touch Docker or adopt the
# real config, so there's no race.
systemctl stop owntone.service < /dev/null 2>/dev/null || true

# --- Only now that the native package is confirmed installed, tear down ----
# --- the old Docker deployment. If dpkg -i had failed above, set -e would --
# --- have aborted the script with Docker still running -- no outage window.
if [ "$DOCKER_RUNNING" = true ]; then
  log "Stopping and disabling the Docker container (compose project at $DOCKER_COMPOSE_DIR)"
  if [ -f "$DOCKER_COMPOSE_DIR/docker-compose.yml" ]; then
    (cd "$DOCKER_COMPOSE_DIR" && docker compose down)
  else
    docker stop "$DOCKER_CONTAINER_NAME"
  fi
  docker update --restart=no "$DOCKER_CONTAINER_NAME" 2>/dev/null || true
else
  log "No running Docker container to tear down"
fi

# --- Adopt the preserved config, but only once -------------------------------
# After the first adoption, /etc/owntone.conf is yours: re-running this script
# (e.g. for an upgrade) never overwrites it again, so anything you've tuned
# since (channels, directories, db_path, ...) survives.
ADOPTED_MARKER=/etc/.owntone-conf-adopted
if [ -f "$ADOPTED_MARKER" ]; then
  log "Config was already adopted on a previous run — leaving your current /etc/owntone.conf as-is"
elif [ -f /etc/owntone.conf.from-docker ]; then
  log "Adopting the preserved Docker config as the active owntone.conf (first run only)"
  cp -a /etc/owntone.conf.from-docker /etc/owntone.conf
  fix_conf_logfile /etc/owntone.conf
  check_conf_paths /etc/owntone.conf
  touch "$ADOPTED_MARKER"
fi

log "Enabling and starting owntone.service"
systemctl enable owntone.service < /dev/null
systemctl restart owntone.service < /dev/null

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
