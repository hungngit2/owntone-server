# Configurable AirPlay/RAOP resync interval

## Problem

Playing to two independent AirPlay 1 speakers set up as a manual L/R stereo
pair (see `settings.webinterface`/devices per-speaker `offset_ms`/`channels`
controls, already shipped), the user hears an audible gap/glitch on one
speaker after extended playback, even after correcting the static per-device
latency offset.

Root cause investigation (this session, read-only, no code changes yet):
OwnTone already re-synchronizes AirPlay/RAOP timing periodically throughout
playback via RTCP-style sync packets (`rtp_sync_is_time()` /
`rtp_sync_packet_next()` in `src/outputs/rtp_common.c`), not only at stream
start. The interval is controlled by `sync_each_nsamples`
(`struct rtp_session`, `src/outputs/rtp_common.h`), which both
`src/outputs/raop.c:1899` and `src/outputs/airplay.c:1183` currently hardcode
to `0` when calling `rtp_session_new()` — per that function's own fallback
(`src/outputs/rtp_common.c:92-93`), `0` means "once per `quality->sample_rate`
samples," i.e. once per second of audio at any sample rate.

If a specific AirPlay receiver's own clock/DAC can't resample smoothly to
absorb a periodic correction (common on cheaper/third-party AirPlay 1
chipsets — real Apple hardware and better implementations usually do this
inaudibly), it may skip or repeat a chunk of audio to catch up each time,
which is audible as a periodic gap. There is no way today to make this
interval tighter (smaller, more frequent, less perceptible corrections) to
test whether that improves things, short of editing and recompiling the
hardcoded constant.

**Important:** this is an experiment based on a plausible but unproven
hypothesis about specific hardware behavior. This feature makes the interval
tunable so the user can test it against their real speakers; it is not a
guaranteed fix for the audible gap.

## Scope

- One new hot-reloadable setting, `misc.airplay_sync_interval_ms`, added via
  the existing `settings.c` DB-backed settings mechanism (same pattern as
  `services.youtube_api_key`, `webinterface.auth_username`, etc.) — no new
  backend route needed, it's auto-exposed via the existing generic
  `/api/settings/misc/airplay_sync_interval_ms` GET/PUT/DELETE endpoints.
- `raop.c` and `airplay.c` read this setting (instead of the hardcoded `0`)
  when creating an RTP session, convert it from milliseconds to samples using
  that stream's own sample rate, and clamp it to a safe range at the point of
  use.
- One new field in the existing Settings > Devices page
  (`web-src/src/pages/PageSettingsDevices.vue`), which already hosts the
  per-device `offset_ms`/`channels` controls this pairs with conceptually.
- **Global, not per-device.** A single value applies to every AirPlay/RAOP
  output; other output types (ALSA, Chromecast, etc.) don't call
  `rtp_session_new()` at all and are entirely unaffected regardless of scope.
  Deliberate YAGNI: this is a first pass at testing an unproven hypothesis,
  not a mainstream per-speaker tuning surface. If tightening the interval
  measurably helps, extending it to a per-device setting (mirroring how
  `offset_ms`/`channels` already work per-speaker) is a reasonable follow-up,
  out of scope here.
- Out of scope: any change to the sync/timestamp *algorithm* itself, any
  change to `offset_ms`/`channels` behavior, any change to non-RAOP/AirPlay
  outputs, per-device granularity for this new setting.

## Design

### Backend (`src/settings.c`)

Add to the existing `misc_options[]` array (alongside
`streamurl_keywords_artwork_url`/`streamurl_keywords_length`, which are
already backend-only technical settings with no bespoke validation):

```c
{ "airplay_sync_interval_ms", SETTINGS_TYPE_INT, { 1000 } },
```

Default `1000` (ms) exactly reproduces today's behavior (one sync per second)
since `rtp_session_new()`'s own `0`-means-`sample_rate`-samples fallback is
mathematically equivalent to "once per second" at any sample rate.

No new validation guard is added to the generic
`jsonapi_reply_settings_option_put()` handler for this option — consistent
with this fork's established preference (documented in an earlier session's
design decision) to keep the generic settings API generic rather than
special-case individual options there. Range clamping instead happens at the
one place the value is actually used (see below), so an out-of-range stored
value can never produce a pathological live interval — it just gets clamped
when read.

### Backend (`src/outputs/raop.c`, `src/outputs/airplay.c`)

At the two `rtp_session_new()` call sites
(`raop.c:1899`, `airplay.c:1183`), replace the hardcoded `0` argument with a
computed sample count:

1. Read `misc.airplay_sync_interval_ms` via `SETTINGS_GETINT("misc",
   "airplay_sync_interval_ms")`.
2. Clamp to `[100, 10000]` (ms). A value of `0` or unset falls back to the
   settings option's own compile-time default (`1000`) via the existing
   `settings_option_getint()` behavior, so clamping only needs to guard
   against a stored value outside the sane range, not against "unset."
3. Convert to samples: `sync_each_nsamples = clamped_ms * quality->sample_rate
   / 1000` — the same `ms * sample_rate / 1000` shape already used
   immediately nearby for `offset_ms → offset_samples` in both files
   (`raop.c:2212`, `airplay.c:1607`), so this follows an established,
   file-local convention rather than introducing a new shared helper (which
   the existing `offset_ms` conversion also doesn't use, despite being
   duplicated across both files the same way).

Like `offset_ms`, this only affects a stream from its next connection
onward — it cannot change a session's sync interval while already playing.

### Frontend (`web-src/src/pages/PageSettingsDevices.vue`)

New `content-with-heading` section (after the existing "Speaker pairing and
device verification" section), with:

- A single `ControlSettingIntegerField` bound to
  `settingsStore.get('misc', 'airplay_sync_interval_ms')`, matching the
  existing `recently_added_limit` field's pattern in
  `PageSettingsWebinterface.vue` (plain autosave-on-input — this is a
  non-sensitive numeric tuning value, not a credential, so the explicit-save
  pattern built for the auth fields doesn't apply here).
- Bounds: min `100`, max `10000`, step `100`, matching the backend clamp.
- Help text explaining what the setting does, that lower values may reduce
  (but aren't guaranteed to eliminate) audible periodic gaps on some AirPlay
  speakers at the cost of more frequent resync traffic, and that changes only
  take effect the next time each device's stream (re)starts — mirroring the
  existing per-device offset field's "restart the stream" note already in
  the UI.
- New i18n keys (heading title, info/help text, field label) added to all 5
  locales (`en`, `de`, `fr`, `zh-CN`, `zh-TW`) with real per-language
  translations, matching this fork's established i18n convention.

## Testing

- No automated test harness exists for either the RAOP/AirPlay C output code
  or Vue settings pages in this project (established fact from this
  session's prior work) — verification is code review plus manual testing.
- Manual verification: set the value via the Settings > Devices field (or
  curl against the generic settings API), restart playback to the affected
  AirPlay speaker(s), and listen for whether tightening the interval changes
  the frequency/audibility of the gap. This is the user's own real-hardware
  test, not something verifiable in this development environment.
