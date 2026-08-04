# Configurable AirPlay/RAOP Sync Interval Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the AirPlay/RAOP periodic resync interval (currently hardcoded to "once per second") a hot-reloadable setting the user can tune, plus a Settings > Devices UI field for it.

**Architecture:** Add one new `settings.c` option (`misc.airplay_sync_interval_ms`), auto-exposed via the existing generic `/api/settings` API. `raop.c` and `airplay.c` each read it (clamped) and convert ms → samples when creating an RTP session, replacing the hardcoded `0` argument passed to `rtp_session_new()`. A new field on the existing Settings > Devices page lets the user change it without SSH/curl.

**Tech Stack:** C (existing `settings.c`/`settings.h` DB-backed settings mechanism, libevent-based RTP output code), Vue 3 `<script setup>`, Pinia (`useSettingsStore`), vue-i18n.

## Global Constraints

- New setting: `misc.airplay_sync_interval_ms`, type INT, default `1000` (ms) — this default must reproduce today's exact behavior (once per second of audio), since `rtp_session_new()`'s own fallback for `sync_each_nsamples <= 0` is `quality->sample_rate` samples, i.e. exactly one second at any sample rate.
- No new validation guard in the generic `jsonapi_reply_settings_option_put()`/`jsonapi_reply_settings_option_delete()` handlers — this fork's established preference (from a prior session's design decision) is to keep that endpoint fully generic, not special-case individual options there.
- Value is clamped to `[100, 10000]` (ms) only at the point of use (when computing samples for `rtp_session_new()`), never at the settings-write layer.
- Scope is global (one value for all AirPlay/RAOP outputs), not per-device. Do not add per-device storage, API, or UI for this — that's explicitly out of scope per the spec.
- Only `src/outputs/raop.c` and `src/outputs/airplay.c` are touched on the backend (both call `rtp_session_new()`); no other output type (ALSA, Chromecast, etc.) is affected or touched.
- Follow existing code style: 2-space indent, brace-on-own-line C style; Vue 3 Composition API `<script setup>` style already used in `PageSettingsDevices.vue`.
- i18n: add real (non-English-copy) translations to all 5 locale files (`en.json`, `de.json`, `fr.json`, `zh-CN.json`, `zh-TW.json`) for every new key, matching the tone of existing entries in each file.
- No automated test harness exists for either the C output code or Vue pages in this project — verification is by careful code reading and, ultimately, the user's own manual hardware test (out of scope for these tasks; noted as Task 3, not a subagent task).

---

### Task 1: Backend — add the setting and wire it into RAOP/AirPlay

**Files:**
- Modify: `src/settings.c:57-61` (`misc_options[]`)
- Modify: `src/outputs/raop.c` (add include, add helper function, change one call site at line ~1899)
- Modify: `src/outputs/airplay.c` (add include, add helper function, change one call site at line ~1183)

**Interfaces:**
- Consumes: existing `SETTINGS_GETINT(category, name)` macro (`src/settings.h:62`, expands to `settings_option_getint(settings_option_get(settings_category_get(category), name))`) — already used elsewhere in this codebase the same way.
- Produces: a new settings option readable via `SETTINGS_GETINT("misc", "airplay_sync_interval_ms")`, and a new static helper `sync_each_nsamples_get(struct media_quality *quality)` defined separately (duplicated, not shared) in both `raop.c` and `airplay.c` — later tasks don't depend on this helper's name since nothing outside these two files calls it.

- [ ] **Step 1: Add the new setting**

In `src/settings.c`, change:

```c
static struct settings_option misc_options[] =
  {
      { "streamurl_keywords_artwork_url", SETTINGS_TYPE_STR },
      { "streamurl_keywords_length", SETTINGS_TYPE_STR },
  };
```

to:

```c
static struct settings_option misc_options[] =
  {
      { "streamurl_keywords_artwork_url", SETTINGS_TYPE_STR },
      { "streamurl_keywords_length", SETTINGS_TYPE_STR },
      { "airplay_sync_interval_ms", SETTINGS_TYPE_INT, { 1000 } },
  };
```

- [ ] **Step 2: Wire it into `src/outputs/raop.c`**

Add the include near the other feature includes (alongside the existing `#include "conffile.h"` at line 62):

```c
#include "settings.h"
```

Immediately before the `master_session_make()` function (search for `static struct raop_master_session *\nmaster_session_make`), add:

```c
// Converts the 'misc.airplay_sync_interval_ms' setting to a sample count for
// rtp_session_new(), clamped to a sane range so a bad stored value can't
// produce a pathological live resync interval. Default (1000ms) reproduces
// today's fixed "once per second" behavior exactly, since rtp_session_new()'s
// own <= 0 fallback is quality->sample_rate samples (= 1 second, any rate).
static int
sync_each_nsamples_get(struct media_quality *quality)
{
  int interval_ms;

  interval_ms = SETTINGS_GETINT("misc", "airplay_sync_interval_ms");
  if (interval_ms < 100)
    interval_ms = 100;
  else if (interval_ms > 10000)
    interval_ms = 10000;

  return interval_ms * quality->sample_rate / 1000;
}
```

Then change the `rtp_session_new()` call inside `master_session_make()` from:

```c
  rms->rtp_session = rtp_session_new(quality, RAOP_PACKET_BUFFER_SIZE, 0, 0);
```

to:

```c
  rms->rtp_session = rtp_session_new(quality, RAOP_PACKET_BUFFER_SIZE, sync_each_nsamples_get(quality), 0);
```

- [ ] **Step 3: Wire it into `src/outputs/airplay.c`**

Add the same include near the other includes (alongside the existing `#include "conffile.h"` at line 45):

```c
#include "settings.h"
```

Immediately before the `master_session_make()` function in this file (search for `static struct airplay_master_session *\nmaster_session_make`), add the identical helper (same body, same name — this file already duplicates the analogous `offset_ms → offset_samples` conversion from `raop.c` rather than sharing a helper, so follow that existing convention):

```c
// Converts the 'misc.airplay_sync_interval_ms' setting to a sample count for
// rtp_session_new(), clamped to a sane range so a bad stored value can't
// produce a pathological live resync interval. Default (1000ms) reproduces
// today's fixed "once per second" behavior exactly, since rtp_session_new()'s
// own <= 0 fallback is quality->sample_rate samples (= 1 second, any rate).
static int
sync_each_nsamples_get(struct media_quality *quality)
{
  int interval_ms;

  interval_ms = SETTINGS_GETINT("misc", "airplay_sync_interval_ms");
  if (interval_ms < 100)
    interval_ms = 100;
  else if (interval_ms > 10000)
    interval_ms = 10000;

  return interval_ms * quality->sample_rate / 1000;
}
```

Then change the `rtp_session_new()` call inside `master_session_make()` from:

```c
  ams->rtp_session = rtp_session_new(quality, AIRPLAY_PACKET_BUFFER_SIZE, 0, clock_id);
```

to:

```c
  ams->rtp_session = rtp_session_new(quality, AIRPLAY_PACKET_BUFFER_SIZE, sync_each_nsamples_get(quality), clock_id);
```

- [ ] **Step 4: Compile-check on the real host (macOS dev env lacks Linux-only headers used elsewhere in this codebase)**

Run (over SSH, from the repo checked out on chainedbox, or any Linux box with the project's build deps):
```bash
make -C src owntone 2>&1 | tail -40
```
Expected: no new warnings/errors introduced by this change (pre-existing warnings elsewhere are not this task's concern). If no Linux host is available, state that clearly in the report and rely on careful code review instead — do not skip verification silently.

- [ ] **Step 5: Manual verification of the default-preserves-behavior claim**

Over SSH against the real host, confirm the new setting reads back the expected default with no DB row present yet:
```bash
curl -s http://127.0.0.1:3689/api/settings/misc/airplay_sync_interval_ms
```
Expected: JSON containing `"value": 1000` (or equivalent showing the compile-time default of `1000` is served when unset).

- [ ] **Step 6: Commit**

```bash
git add src/settings.c src/outputs/raop.c src/outputs/airplay.c
git commit -m "Make AirPlay/RAOP resync interval a configurable setting"
```

---

### Task 2: Frontend — Settings > Devices field

**Files:**
- Modify: `web-src/src/pages/PageSettingsDevices.vue`
- Modify: `web-src/src/i18n/en.json`, `web-src/src/i18n/de.json`, `web-src/src/i18n/fr.json`, `web-src/src/i18n/zh-CN.json`, `web-src/src/i18n/zh-TW.json`

**Interfaces:**
- Consumes: `ControlSettingIntegerField` (`web-src/src/components/ControlSettingIntegerField.vue` — takes `disabled`/`placeholder`/`setting` props only; it has no `min`/`max`/`step` props, unlike the different `ControlIntegerField` component already used elsewhere on this same page for the per-device offset — do not conflate the two components), `useSettingsStore().get(categoryName, optionName)` → `{ category, name, value, ... }` (`web-src/src/stores/settings.js`, unchanged), and the existing `ContentWithHeading`/`PaneTitle` components already imported in this file.
- Produces: nothing consumed by later tasks — this is the final task.

- [ ] **Step 1: Add the new Settings section**

In `web-src/src/pages/PageSettingsDevices.vue`, add a new `content-with-heading` block after the existing "Speaker pairing and device verification" block (i.e., right before the closing `</template>` of the main template):

```vue
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.devices.sync-tuning') }" />
    </template>
    <template #content>
      <div
        class="content"
        v-text="$t('settings.devices.sync-tuning-info')"
      />
      <control-setting-integer-field
        :setting="settingsStore.get('misc', 'airplay_sync_interval_ms')"
      />
    </template>
  </content-with-heading>
```

And add the new import + store usage in the `<script setup>` block. Change:

```js
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import ControlDropdown from '@/components/ControlDropdown.vue'
import ControlIntegerField from '@/components/ControlIntegerField.vue'
import ControlPinField from '@/components/ControlPinField.vue'
import ControlSwitch from '@/components/ControlSwitch.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import outputs from '@/api/outputs'
import { ref } from 'vue'
import remotes from '@/api/remotes'
import { useOutputsStore } from '@/stores/outputs'
import { useRemotesStore } from '@/stores/remotes'

const outputsStore = useOutputsStore()
const remotesStore = useRemotesStore()
```

to:

```js
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import ControlDropdown from '@/components/ControlDropdown.vue'
import ControlIntegerField from '@/components/ControlIntegerField.vue'
import ControlPinField from '@/components/ControlPinField.vue'
import ControlSettingIntegerField from '@/components/ControlSettingIntegerField.vue'
import ControlSwitch from '@/components/ControlSwitch.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import outputs from '@/api/outputs'
import { ref } from 'vue'
import remotes from '@/api/remotes'
import { useOutputsStore } from '@/stores/outputs'
import { useRemotesStore } from '@/stores/remotes'
import { useSettingsStore } from '@/stores/settings'

const outputsStore = useOutputsStore()
const remotesStore = useRemotesStore()
const settingsStore = useSettingsStore()
```

(import ordering here matches this file's existing convention: single-specifier imports sorted alphabetically by local name, `{ ref }` sorting into the same list per this project's `sort-imports` ESLint rule — verify with the lint step below rather than assuming.)

- [ ] **Step 2: Add i18n keys**

`ControlSetting.vue` (the shared base component `ControlSettingIntegerField` wraps) derives a field's visible label automatically from `settings.<option.category>.<option.name-with-dashes>` — since this option's category is `"misc"` (not `"devices"`), its label key MUST be under `settings.misc`, even though the heading/info text above live under `settings.devices` (matching this page's existing key namespace for hand-written copy). Both namespaces are needed.

In `web-src/src/i18n/en.json`, inside the existing `"settings": { "devices": { ... } }` object (around line 372), add two keys (alphabetically: `sync-tuning` and `sync-tuning-info` sort after `speaker-pairing-info` and before `verification-code`):

```json
      "speaker-pairing-info": "If your speaker requires pairing, enter the verification code displayed. You can also set an offset compensation (±2000 ms) to synchronise playback between multiple speakers. To apply the offset, please restart the stream.",
      "sync-tuning": "AirPlay/RAOP Sync Tuning",
      "sync-tuning-info": "How often (in milliseconds) OwnTone resynchronizes AirPlay/RAOP playback timing per device (100–10000, default 1000). Lower values correct drift faster but may cause more frequent audible adjustments on some speakers. Takes effect the next time playback starts on each device.",
      "verification-code": "Verification code"
```

There is currently no `"settings": { "misc": { ... } }` object in any locale file — add one. In `web-src/src/i18n/en.json`, inside the top-level `"settings": { ... }` object, insert a new `"misc"` section alphabetically between `"devices"` and `"services"` (around line 380, right after the `"devices"` object's closing `},`):

```json
    "misc": {
      "airplay-sync-interval-ms": "Resync interval (ms)"
    },
```

- [ ] **Step 3: Add the same keys to the other 4 locales with real translations**

In `web-src/src/i18n/de.json`, `fr.json`, `zh-CN.json`, `zh-TW.json`: add the same two `devices.*` keys and the same `misc` object (same structure, same insertion points), with real per-language translations matching each file's existing tone (read a few neighboring keys first, e.g. `devices.speaker-pairing-info`, to match register/formality):

- `de.json`: `"sync-tuning": "AirPlay/RAOP-Synchronisationsabstimmung"`, `"sync-tuning-info": "Wie oft (in Millisekunden) OwnTone die AirPlay/RAOP-Wiedergabezeit pro Gerät neu synchronisiert (100–10000, Standard 1000). Niedrigere Werte korrigieren Drift schneller, können bei manchen Lautsprechern aber zu häufigeren, hörbaren Anpassungen führen. Wird erst beim nächsten Start der Wiedergabe auf jedem Gerät wirksam."`, misc: `"airplay-sync-interval-ms": "Resync-Intervall (ms)"`
- `fr.json`: `"sync-tuning": "Réglage de la synchronisation AirPlay/RAOP"`, `"sync-tuning-info": "Fréquence (en millisecondes) à laquelle OwnTone resynchronise la synchronisation de lecture AirPlay/RAOP par appareil (100–10000, valeur par défaut 1000). Des valeurs plus faibles corrigent la dérive plus rapidement mais peuvent provoquer des ajustements audibles plus fréquents sur certaines enceintes. Prend effet au prochain démarrage de la lecture sur chaque appareil."`, misc: `"airplay-sync-interval-ms": "Intervalle de resynchronisation (ms)"`
- `zh-CN.json`: `"sync-tuning": "AirPlay/RAOP 同步调节"`, `"sync-tuning-info": "OwnTone 每隔多少毫秒重新同步各设备的 AirPlay/RAOP 播放时序（100–10000，默认 1000）。数值越小，纠正漂移越快，但在某些音箱上可能导致更频繁的可听调整。更改仅在每台设备下次开始播放时生效。"`, misc: `"airplay-sync-interval-ms": "重新同步间隔（毫秒）"`
- `zh-TW.json`: `"sync-tuning": "AirPlay/RAOP 同步調整"`, `"sync-tuning-info": "OwnTone 每隔多少毫秒重新同步各裝置的 AirPlay/RAOP 播放時序（100–10000，預設 1000）。數值越小，校正飄移越快，但在某些喇叭上可能導致更頻繁的可聽調整。變更僅在每台裝置下次開始播放時生效。"`, misc: `"airplay-sync-interval-ms": "重新同步間隔（毫秒）"`

- [ ] **Step 4: Lint**

```bash
cd web-src && npm run lint
```
Expected: no errors. Fix any `sort-imports`/`capitalized-comments`/other issues following this project's established conventions (match surrounding code style exactly rather than disabling rules, unless a rule is a known false positive already documented elsewhere in this codebase).

- [ ] **Step 5: Build**

```bash
npm run build
```
Expected: build succeeds, `htdocs/assets/index.js`/`index.css` regenerated (commit them alongside source, matching this project's established practice of committing prebuilt assets).

- [ ] **Step 6: Manual verification**

Run the web dev server (or deploy to chainedbox) and, in a browser, navigate to `/#/settings/devices`. Confirm:
1. A new "AirPlay/RAOP Sync Tuning" section renders below the speaker pairing section, with the info text and an integer input showing `1000` (the default) when nothing has been configured yet.
2. Changing the value and confirming it saves (no error icon) via the browser's network tab or by reloading the page and seeing the new value persisted.

- [ ] **Step 7: Commit**

```bash
git add web-src/src/pages/PageSettingsDevices.vue web-src/src/i18n/en.json web-src/src/i18n/de.json \
  web-src/src/i18n/fr.json web-src/src/i18n/zh-CN.json web-src/src/i18n/zh-TW.json \
  htdocs/assets/index.js htdocs/assets/index.css
git commit -m "Add AirPlay/RAOP sync interval field to Settings > Devices"
```

---

### Task 3: Manual end-to-end hardware verification (blocking — needs user + real speakers)

Not a subagent task. After Tasks 1-2 are merged and deployed:

- [ ] User sets a tighter `airplay_sync_interval_ms` value (e.g. `250`) via the new Settings > Devices field, restarts playback to the affected AirPlay speaker(s), and listens for whether the periodic gap becomes less frequent/audible.
- [ ] This is genuinely uncertain (see spec) — report back whichever result (helped / no change / worse) so the default and this feature's future (e.g. per-device granularity) can be decided with real data.
