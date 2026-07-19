# OwnTone

OwnTone is a media server that lets you play audio sources such as local files,
Spotify, pipe input or internet radio to AirPlay 1 and 2 receivers, Chromecast
receivers, Roku Soundbridge, a browser or the server’s own sound system. Or you
can listen to your music via any client that supports mp3 streaming.

You control the server via a web interface, Apple Remote, an Android remote
(e.g. Retune), an MPD client, json API or DACP.

OwnTone also serves local files via the Digital Audio Access Protocol (DAAP) to
iTunes (Windows), Apple Music (macOS) and Rhythmbox (Linux), and via the Roku
Server Protocol (RSP) to Roku devices.

Runs on Linux, BSD and macOS.

OwnTone was previously called forked-daapd, which again was a rewrite of
mt-daapd (Firefly Media Server).


## This fork

This fork adds a per-output **channel selector** (both/left/right), so two
outputs (e.g. two AirPlay speakers) can be configured as a real stereo pair —
one playing the left channel, the other the right. Configure it per output in
`owntone.conf` (`channels = "unset" | "left" | "right"` in an `audio {}`,
`alsa "name" {}`, `airplay "name" {}`, or `fifo {}` block), via the web UI's
outputs panel, or via the JSON API (`"channels"` field on `/api/outputs`).

It also adds a native `arm64` build/deploy pipeline for Debian-based hosts
(e.g. Armbian), as an alternative to the official Docker image:

- **GitHub Actions** (`.github/workflows/release-arm64.yml`) builds a `.deb`
  and publishes it as a release asset on every `v*` tag push.
- **`install.sh`** is a one-click migration script for a host currently
  running OwnTone via Docker — it safely stops the container (checking
  playback state first), preserves your existing config, and installs the
  native package:
  ```
  curl -fsSL https://raw.githubusercontent.com/hungngit2/owntone-server/master/install.sh | sudo bash
  ```
  Safe to re-run for upgrades; only ever adopts your Docker config once, so
  anything you've since tuned in `/etc/owntone.conf` is never overwritten.

It also adds optional **per-stream custom HTTP request headers** for plain
`http(s)://` queue items (`DATA_KIND_HTTP`) — useful for URLs that require a
specific header (e.g. a `Referer`) to be fetched at all, such as direct CDN
URLs resolved via `yt-dlp -g`. Pass an already-formatted `"Key: Value\r\nKey2:
Value2"` string as `headers` alongside `uris` on `POST /api/queue/items/add`,
or set/change it on an already-queued item via `PUT /api/queue/items/{id}`.
Plain URLs with no `headers` behave exactly as before — this is opt-in only.


## Looking for help?

Visit the [OwnTone documentation](https://owntone.github.io/owntone-server/) for
usage and set up instructions, API documentation, etc.

If you are looking for information on how to get and install OwnTone, then see
the [Installation](https://owntone.github.io/owntone-server/installation/)
instructions.
