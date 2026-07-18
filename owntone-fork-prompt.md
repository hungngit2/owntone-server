# Prompt: add per-output channel selection to OwnTone

Paste this into an AI coding session with the forked OwnTone repo checked
out and available.

---

I want to add a per-output audio channel selector to OwnTone: each
configured output (ALSA, AirPlay/RAOP, Chromecast, etc.) should be able
to play **both channels (default/current behavior)**, **left channel
only**, or **right channel only** — independently of every other output.

## Why

The real use case: two independent AirPlay speakers used as a stereo
pair, one getting the left channel and one getting the right, so they
act like a single stereo speaker instead of both playing the identical
full mix. Right now that requires an external PipeWire workaround
(separate loopback device, RAOP relinking, mixer hacks) because OwnTone
has no concept of "which channel(s) should this specific output play."
Doing it natively removes all of that — one real synchronized audio
path per speaker instead of two independent RAOP sessions that drift out
of sync over time (confirmed in practice — see
`docs/pipewire-stereo-split-plan.md` in the php-ytb-tone dashboard repo
this is paired with, if it's available to you, for the full backstory).

## Known risk carried over from the PipeWire workaround: idle drift

Confirmed empirically with the PipeWire workaround: **two independent
AirPlay/RAOP sessions (one per speaker) gradually drift out of L/R sync
the longer they sit connected, especially noticeable after the speakers
have been idle/standing by for a while** — each session has its own
clock with no shared reference. A full reconnect (there, toggling the
output off/on) resets it. The workaround for now is polling for idle
time and force-reconnecting after ~15 minutes of no playback (see
`docs/pipewire-stereo-split-plan.md`'s "Auto-resync" section) — a
band-aid, not a fix.

**This same problem will very likely still exist with the native
channel-toggle feature**, if each speaker ends up as its own separate
`airplay` output config in `owntone.conf` (which is the natural way to
implement per-output channel selection) — the channel-toggle feature
only removes the need for external PipeWire plumbing, it doesn't by
itself give the two outputs a shared clock. Before assuming this is
solved by going native, investigate:
- Does OwnTone already have any shared-timing/sync logic across
  *multiple simultaneously active* outputs? The existing `sync_disable`/
  `adjust_period_seconds` options on the local ALSA output (seen in
  `owntone.conf`) suggest OwnTone is at least aware of drift-correction
  for keeping local audio in sync with AirPlay — check whether that
  logic (or something like it) could be extended to keep two AirPlay
  outputs mutually in sync with each other, not just one output in sync
  with wall-clock/local audio.
- If no such shared-clock mechanism exists, decide whether it's worth
  building one (nontrivial — real audio sync work) vs. carrying the same
  idle-based reconnect band-aid forward natively (much cheaper, but only
  a mitigation, not a real fix).
Report what you find here too, before committing to an approach.

## What to investigate first (don't start coding yet)

1. Find where decoded PCM audio gets distributed to each active output.
   My guess, based on log tags observed at runtime (`player:`,
   `laudio:`, `outputs:`, `xcode:`), is something like `src/player.c`
   and/or `src/outputs.c`, with per-backend files like `src/laudio.c`
   (ALSA), `src/raop.c` or similar (AirPlay), `src/cast.c` (Chromecast).
   Confirm the actual structure — don't trust my guess blindly.
2. Identify the exact point where the **same** PCM buffer is hand ed off
   to multiple output backends, and confirm each backend does its own
   resampling/encoding *after* that point (this matters: the channel
   transform needs to happen once, centrally, before that divergence —
   not duplicated per-backend).
3. Find the existing per-output config struct/section (used for things
   like `nickname`, `mixer`, `mixer_device` per output in `owntone.conf`)
   and the output-selection JSON API handler (`/api/outputs`,
   `PUT /api/outputs/<id>`) that already exposes `selected`/`volume` per
   output — the new channel field should follow the same pattern.

Report back what you find (file names, function names, the actual
shared-PCM-buffer struct/type) before writing any code, so we can agree
on the insertion point.

## What to implement, once the above is confirmed

1. **Core transform**: given a PCM buffer with N channels (assume
   stereo, 16-bit interleaved, matching what's already used elsewhere in
   the codebase) and a mode (`both` / `left` / `right`), produce an
   output buffer where:
   - `both`: unchanged (current behavior).
   - `left`: right channel replaced with a copy of the left channel
     (so a mono-capable speaker gets the left content on both its
     inputs, not silence on one side).
   - `right`: left channel replaced with a copy of the right channel.
   This should be a small, allocation-free, per-sample function — treat
   it like existing resampling/format-conversion code already in the
   codebase for style/conventions.
2. **Config**: add a `channels = "both" | "left" | "right"` option to
   each per-output config block (`audio {}`, `alsa "name" {}`,
   `airplay "name" {}`, etc. — wherever `nickname`/`mixer` currently
   live). Default to `both` if unset, to keep existing configs
   unchanged.
3. **Runtime**: apply the transform for a given output right before that
   output's own encode/resample step, using its configured mode.
4. **JSON API**: add a `"channels"` field to the output objects in
   `GET /api/outputs`, and accept it in `PUT /api/outputs/<id>` (same
   validation/error-handling pattern as the existing `selected`/`volume`
   fields there).
5. **Web UI** (if in scope — ask me first, this might be a separate
   follow-up): a per-output control (radio buttons or a 3-way toggle)
   next to the existing volume slider for each output in the outputs
   panel.

## Build + deploy: GitHub Actions + one-click install.sh for Armbian

Once the feature above is implemented, also set up how it actually gets
built and installed on the target host (an Armbian SBC, `aarch64`,
currently running upstream OwnTone via the official `owntone/owntone`
Docker image — see `docs/pipewire-stereo-split-plan.md` for the host's
full profile, including that it's severely memory-constrained, under
1GB RAM). This is a **native binary/`.deb` package build, not a Docker
image** — a deliberate departure from the current Docker-based
deployment.

**Why native instead of Docker, worth knowing:** a large fraction of the
pain documented in `docs/pipewire-stereo-split-plan.md`'s gotchas came
specifically from the container boundary — Docker masking
`/proc/asound` from the container (breaking OwnTone's own ALSA
card-index lookups), needing `privileged: true` to work around it,
explicit `/dev/snd` device passthrough, etc. A native install removes
that whole boundary — OwnTone talks to ALSA directly as a normal host
process, no masking, no `privileged: true`, no device passthrough
config. That's a real simplification on top of the channel-toggle
feature itself, not just a packaging preference.

### GitHub Actions workflow

- Trigger on tag push (e.g. `v*`) and via `workflow_dispatch` for manual
  builds.
- Build for `arm64`/`aarch64` specifically (this host's architecture).
  Prefer a native `arm64` GitHub-hosted runner if available on the plan
  in use (e.g. `ubuntu-24.04-arm` or equivalent) over cross-compiling or
  QEMU-emulated builds — much simpler and faster if available; fall back
  to QEMU-based emulated build only if it isn't.
- Investigate OwnTone's actual build system first (autotools/meson/
  whatever it turns out to be — don't assume) and its real dependency
  list (likely libavcodec/libavformat, libevent, libconfuse, libsqlite3,
  libavahi, libplist, gcrypt/openssl, etc. — confirm against the actual
  `configure`/build docs, don't guess at exact package names).
- Package the built binary + a systemd unit + a default `owntone.conf`
  into a `.deb` (preferred, since Armbian is Debian-based) — or a plain
  tarball if a proper `.deb` build turns out to be significantly more
  work than it's worth; note which you chose and why.
- Publish the package as a GitHub Release asset, tagged with the version.

### `install.sh`

One-click script, run as root on the Armbian host, that:
- Detects the current architecture and downloads the matching latest (or
  a pinned) release asset from the GitHub repo via the GitHub API/`gh`
  CLI — don't hardcode a version if avoidable.
- **Safely migrates off the existing Docker deployment**: checks whether
  the `owntone` Docker container is currently running, and if it is,
  confirms nobody's actively listening (same pattern already used
  elsewhere in this project — check the player state via OwnTone's own
  `/api/player` before stopping anything) before stopping/disabling it.
  Never silently discard the existing config/library database — the
  existing bind-mounted config (`/opt/docker/owntone/config/owntone.conf`)
  and library paths should carry over to the native install, not be
  reset to defaults.
- Installs the package (`dpkg -i` + `apt-get install -f` for
  dependencies, or equivalent), sets up and enables a systemd service
  for OwnTone itself.
- Idempotent — safe to re-run for upgrades (stop the running native
  service, install the new package, restart).
- Clear rollback instructions in a comment or printed at the end (how to
  go back to the Docker image if the native build has a problem).

## Constraints

- Don't touch anything unrelated to this feature — no drive-by
  refactors, no unrelated cleanup.
- Match existing code style and conventions exactly (this is someone
  else's C codebase you're extending, not greenfield).
- Default behavior must be unchanged for every existing config that
  doesn't set `channels` — this cannot be a breaking change for current
  users.
- Flag anywhere you're not sure the transform composes correctly with
  existing per-output resampling/quality-matching logic — that's the
  part most likely to have a subtle bug (e.g. if resampling happens
  before your transform in one backend and after in another, the buffer
  shape assumptions will differ).
- I can only test this on real hardware (two physical AirPlay speakers)
  once it's built — don't claim it works until I've confirmed it audibly
  splits correctly on both channels.
