# Armbian Native Build + Deploy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build OwnTone as a native arm64 `.deb` via GitHub Actions on tag push, publish it as a release asset, and provide a one-click `install.sh` that migrates the target Armbian host off its current Docker-based OwnTone deployment onto the native package, preserving existing config/library data.

**Architecture:** OwnTone already builds with autotools and already ships an RPM spec (`owntone.spec.in`) that does exactly this shape of packaging (useradd/groupadd in a pre-install script, systemd unit install, DESTDIR-based `make install`) — mirror that pattern for Debian using `debhelper`/`dpkg-buildpackage` instead of `rpmbuild`, since Armbian is Debian-based. CI runs on a native `arm64` GitHub-hosted runner (no QEMU) and uploads the `.deb` to a GitHub Release. `install.sh` reads the *existing* Docker deployment's bind-mounted config/library paths, stops the container safely (checking `/api/player` first, matching this project's own existing "check player state before disrupting playback" pattern), and hands them to the native package.

**Tech Stack:** GitHub Actions, autotools, `debhelper`/`dpkg-buildpackage`, systemd, bash.

## Global Constraints

- Target host ("chainedbox"): Armbian SBC, `aarch64`, **under 1GB total RAM**, already running Home Assistant/Jellyfin/AdGuard Home/Docker at 90%+ swap utilization even at idle. Don't add any new always-on process or service as part of this work — this plan only replaces the existing always-on OwnTone Docker container with an equivalent native systemd service; it doesn't add anything net-new that runs continuously.
- Never silently discard the existing config or library database. The current bind-mounted config lives at `/opt/docker/owntone/config/owntone.conf`.
- `install.sh` must be idempotent (safe to re-run for upgrades) and must not require re-entering config by hand.
- Don't touch anything unrelated — no drive-by refactors of the C code or existing CI workflows in this same work.

---

### Task 1: GitHub Actions release workflow

**Files:**
- Create: `.github/workflows/release-arm64.yml`

**Interfaces:**
- Produces: a `.deb` file uploaded as a GitHub Release asset, named `owntone_<version>_arm64.deb` — consumed by `install.sh` (Task 3), which downloads it by this naming convention via the GitHub Releases API.

- [ ] **Step 1: Confirm a native arm64 GitHub-hosted runner is available on this repo's plan**

Run (requires repo write access / `gh` CLI authenticated):
```bash
gh api /repos/owntone/owntone-server/actions/runners 2>&1 | head -5   # just to confirm API access; the real signal is runner availability, checked next
```
The relevant label is `ubuntu-24.04-arm` (GitHub-hosted native arm64 Ubuntu runner). If this repo/org's plan doesn't have access to it (public repos on GitHub-hosted arm64 runners require the runner to be available on the plan — as of this writing it is available for public repos), fall back to the QEMU-emulated build in Step 1a below instead of Step 1.

- [ ] **Step 1a (fallback only, skip if native arm64 runner works): QEMU cross-build variant**

If `ubuntu-24.04-arm` isn't available, replace the `runs-on`/build steps in Step 2 below with a `docker/setup-qemu-action@v3` + `docker/setup-buildx-action@v3` step, and run the build inside an `arm64v8/ubuntu:24.04` container via `docker run --platform linux/arm64 ...` instead of directly on the runner. Note in the workflow's top comment which mode is active and why, so a future maintainer knows this was a deliberate fallback, not an oversight.

- [ ] **Step 2: Write the workflow**

```yaml
# .github/workflows/release-arm64.yml
name: Release arm64 .deb

permissions:
  contents: write

on:
  push:
    tags:
      - 'v*'
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-24.04-arm

    steps:
      - uses: actions/checkout@v7

      - name: Install build dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -yq build-essential clang clang-tools git autotools-dev autoconf \
            libtool gettext gawk gperf bison flex libconfuse-dev libunistring-dev libsqlite3-dev \
            libavcodec-dev libavformat-dev libavfilter-dev libswscale-dev libavutil-dev libasound2-dev \
            libxml2-dev libgcrypt20-dev libavahi-client-dev zlib1g-dev libevent-dev libplist-dev \
            libsodium-dev libcurl4-openssl-dev libjson-c-dev libprotobuf-c-dev libpulse-dev \
            libwebsockets-dev libgnutls28-dev debhelper devscripts fakeroot

      - name: Determine version
        id: version
        run: |
          VERSION="${GITHUB_REF_NAME#v}"
          echo "version=$VERSION" >> "$GITHUB_OUTPUT"

      - name: Build .deb
        run: |
          autoreconf -vi
          ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
            --enable-chromecast --with-pulseaudio --with-systemddir=/lib/systemd/system
          make -j"$(nproc)"
          fakeroot make install DESTDIR="$PWD/debian/owntone"
          dpkg-deb --build --root-owner-group debian/owntone \
            "owntone_${{ steps.version.outputs.version }}_arm64.deb"

      - name: Upload release asset
        uses: softprops/action-gh-release@v2
        with:
          files: owntone_${{ steps.version.outputs.version }}_arm64.deb
```

(This references `debian/control`/`debian/postinst`/etc from Task 2, which must exist in the repo before this workflow can succeed — do Task 2 first, or in the same PR.)

- [ ] **Step 3: Verify locally before relying on CI**

Since this can't be fully exercised without pushing a tag, at minimum dry-run the build portion on an arm64 machine or arm64 Docker container:
```bash
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src ubuntu:24.04 bash -c '
  apt-get update && apt-get install -yq build-essential autotools-dev autoconf libtool gettext gawk gperf bison flex libconfuse-dev libunistring-dev libsqlite3-dev libavcodec-dev libavformat-dev libavfilter-dev libswscale-dev libavutil-dev libasound2-dev libxml2-dev libgcrypt20-dev libavahi-client-dev zlib1g-dev libevent-dev libplist-dev libsodium-dev libcurl4-openssl-dev libjson-c-dev libprotobuf-c-dev libpulse-dev libwebsockets-dev libgnutls28-dev debhelper devscripts fakeroot &&
  autoreconf -vi &&
  ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var --enable-chromecast --with-pulseaudio --with-systemddir=/lib/systemd/system &&
  make -j"$(nproc)" &&
  fakeroot make install DESTDIR=/src/debian/owntone &&
  dpkg-deb --build --root-owner-group debian/owntone /src/owntone_test_arm64.deb
'
```
Expected: `owntone_test_arm64.deb` produced with no errors. (This uses QEMU under the hood via Docker Desktop's binfmt support, so it's slow — a few minutes — but validates the packaging steps independent of GitHub Actions runner availability.)

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release-arm64.yml
git commit -m "Add arm64 .deb release workflow"
```

---

### Task 2: Debian packaging metadata (`debian/` directory)

**Files:**
- Create: `debian/control`
- Create: `debian/postinst`
- Create: `debian/prerm`
- Create: `debian/postrm`
- Create: `debian/owntone.conffiles`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the package metadata Task 1's `dpkg-deb --build` step packages up; `postinst`'s useradd/systemd-enable logic runs on the *target host* at `dpkg -i` time (not on the CI build runner), mirroring `owntone.spec.in`'s `%pre`/`%post` sections exactly.

**Why not `--enable-install-user` at build time:** that configure flag makes `make install-data-hook` run `useradd`/`chown` immediately — fine for a local `make install`, wrong for packaging, since it would run those commands against the *CI runner's* own user database, not the eventual install target's. `owntone.spec.in` avoids this by leaving user creation to `%pre` (RPM's pre-install scriptlet, executed on the target at install time); `debian/postinst` is the Debian equivalent, so build *without* `--enable-install-user` (already the case in Task 1's `./configure` line) and do the equivalent work here instead.

- [ ] **Step 1: Write `debian/control`**

```
Source: owntone
Section: sound
Priority: optional
Maintainer: OwnTone Fork Maintainer <noreply@example.invalid>
Build-Depends: debhelper-compat (= 13)
Standards-Version: 4.6.0

Package: owntone
Architecture: arm64
Depends: ${shlibs:Depends}, ${misc:Depends}, adduser, avahi-daemon
Description: DAAP/DACP (iTunes), RSP and MPD server with AirPlay/Chromecast support
 OwnTone is a DAAP/DACP (iTunes), MPD (Music Player Daemon) and RSP (Roku)
 media server, with support for AirPlay devices/speakers, Apple Remote,
 MPD clients, Chromecast, network streaming, internet radio, Spotify and
 LastFM.
```

- [ ] **Step 2: Write `debian/postinst`** (mirrors `owntone.spec.in`'s `%pre` + `%post` — see `owntone.spec.in:80-94`)

```bash
#!/bin/sh
set -e

case "$1" in
  configure)
    getent group owntone >/dev/null || groupadd --system owntone
    getent passwd owntone >/dev/null || useradd --system --no-create-home \
      --gid owntone --groups audio --shell /usr/sbin/nologin owntone
    getent group pulse-access >/dev/null && usermod --append --groups pulse-access owntone || true

    mkdir -p /var/log /var/run /var/cache/owntone
    chown owntone:owntone /var/cache/owntone

    if [ ! -f /etc/owntone.conf ]; then
      cp /usr/share/owntone/owntone.conf.default /etc/owntone.conf
    fi

    deb-systemd-helper enable owntone.service >/dev/null || true
    deb-systemd-invoke restart owntone.service >/dev/null || true
    ;;
esac

exit 0
```
(References `/usr/share/owntone/owntone.conf.default` — Task 1's `make install` step already installs the templated `owntone.conf` to `/etc/owntone.conf` directly via its own `install-data-hook`; since that hook is guarded by `COND_INSTALL_CONF_FILE`/won't overwrite an existing file, and packaging shouldn't ship a live config directly into `/etc` for a `.deb` (dpkg conffile handling wants it there via `debian/owntone.conffiles` instead, not copied by a postinst) — reconcile this by checking, at implementation time, exactly where `make install` places the rendered `owntone.conf.in` output relative to `$(DESTDIR)$(sysconfdir)`, and either let `debian/owntone.conffiles` (Step 4) mark `/etc/owntone.conf` as a conffile directly instead of the `postinst` copy shown above, whichever avoids double-handling. Flag this specific reconciliation prominently in the PR — it's the one place this task's design description and the existing Makefile-driven config install could conflict.)

- [ ] **Step 3: Write `debian/prerm` and `debian/postrm`**

```bash
# debian/prerm
#!/bin/sh
set -e

case "$1" in
  remove|deconfigure)
    deb-systemd-invoke stop owntone.service >/dev/null || true
    ;;
esac

exit 0
```

```bash
# debian/postrm
#!/bin/sh
set -e

case "$1" in
  purge)
    deb-systemd-helper purge owntone.service >/dev/null || true
    getent passwd owntone >/dev/null && userdel owntone || true
    rm -rf /var/cache/owntone
    ;;
esac

exit 0
```

- [ ] **Step 4: Mark the config file as a conffile**

```
# debian/owntone.conffiles
/etc/owntone.conf
```

- [ ] **Step 5: Make scripts executable and verify the packaging step runs**

```bash
chmod +x debian/postinst debian/prerm debian/postrm
```
Re-run Task 1 Step 3's local Docker build to confirm `dpkg-deb --build` still succeeds with these files present, then additionally verify with `dpkg-deb --info owntone_test_arm64.deb` that `postinst`/`prerm`/`postrm` are listed among the control members.

- [ ] **Step 6: Commit**

```bash
git add debian/
git commit -m "Add Debian packaging metadata for native arm64 builds"
```

---

### Task 3: `install.sh` — migrate the Armbian host from Docker to native

**Files:**
- Create: `install.sh` (repo root, alongside `owntone.spec.in`/`owntone.service.in`)

**Interfaces:**
- Consumes: the `owntone_<version>_arm64.deb` GitHub Release asset produced by Task 1.

- [ ] **Step 1: Write the script**

```bash
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

  if [ "$PLAYER_STATE" = "play" ]; then
    die "OwnTone is currently playing (state=play) — refusing to stop the Docker container automatically. Pause playback (or re-run with FORCE_STOP=1) and try again."
  fi
  log "Player state is '$PLAYER_STATE' — safe to stop"

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
NATIVE_RUNNING=false
if systemctl is-active --quiet owntone.service 2>/dev/null; then
  NATIVE_RUNNING=true
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
```

- [ ] **Step 2: Shellcheck it**

Run: `shellcheck install.sh`
Expected: no errors (warnings about `curl`/`grep`/`sed` JSON-parsing fragility are acceptable here given no guaranteed `jq` on a minimal Armbian image — but if `jq` turns out to already be present on the target host, prefer it over the `grep`/`sed` fallback for the release-asset lookup; check before assuming).

- [ ] **Step 3: Dry-run the Docker-detection and version-resolution logic without actually installing**

```bash
bash -n install.sh   # syntax check
# Manually exercise the non-destructive branches, e.g. by temporarily commenting out the dpkg -i line,
# on a scratch VM or container that has `docker` and a fake owntone container, to confirm:
#  - it correctly identifies the running container name
#  - it correctly reads and parses a fake /api/player response
#  - it refuses to proceed when state=play
```

- [ ] **Step 4: Commit**

```bash
git add install.sh
git commit -m "Add install.sh for migrating the Armbian host from Docker to native"
```

---

### Task 4: End-to-end verification on the real host (blocking — do not skip)

**Files:** none — verification only.

- [ ] **Step 1: Tag a release and confirm the GitHub Actions workflow succeeds**

```bash
git tag v29.2-channels1
git push origin v29.2-channels1
gh run watch   # or check the Actions tab
```
Expected: `release-arm64.yml` completes, `.deb` asset attached to the release.

- [ ] **Step 2: Run `install.sh` on chainedbox with playback stopped**

Confirm no active playback (`curl http://127.0.0.1:3689/api/player`, check `state` != `play`), then:
```bash
curl -fsSL https://raw.githubusercontent.com/<repo>/master/install.sh | sudo bash
```
(Or copy the script over and run it locally — given this is a memory-constrained host, prefer running it interactively over ssh rather than via a piped-curl one-liner, so output is easy to review live.)

- [ ] **Step 3: Confirm the native service is healthy and the config/library carried over**

```bash
systemctl status owntone
diff /etc/owntone.conf /opt/docker/owntone/config/owntone.conf   # should be identical right after migration
curl -s http://127.0.0.1:3689/api/library | head
```
Expected: service active, config matches the pre-migration Docker config, library still populated (not rescanned from empty).

- [ ] **Step 4: Confirm the Docker container is stopped and not set to restart**

```bash
docker ps -a | grep owntone
```
Expected: container present but stopped, `RestartPolicy` no longer `always`/`unless-stopped`.

- [ ] **Step 5: Re-run `install.sh` to confirm idempotency (upgrade path)**

```bash
sudo ./install.sh
```
Expected: detects Docker already stopped, detects native service already running, stops/reinstalls/restarts cleanly, no errors.

- [ ] **Step 6: Confirm host memory headroom didn't regress**

```bash
free -h
```
Compare against the pre-migration baseline noted in `docs/pipewire-stereo-split-plan.md` (90-99% swap used at idle) — the native install should, if anything, *reduce* pressure (no Docker daemon overhead for this one container), but confirm rather than assume, given how little margin this host has.
