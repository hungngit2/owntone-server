# Per-Output Channel Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let each configured OwnTone output independently play both channels, left-only, or right-only, so two AirPlay speakers can act as a real stereo pair.

**Architecture:** Add a `channels` mode (`both`/`left`/`right`) to `struct output_device`, persist it the same way `offset_ms` is (conffile default + DB-backed runtime override + JSON API), and apply a new allocation-free `channel_transform()` helper at each backend's private-buffer-copy point. RAOP and AirPlay currently encode once per shared "master session" across every device at the same quality, so their master-session cache key must be extended with channel mode — otherwise two same-quality AirPlay devices with different channel modes would silently get identical audio.

**Tech Stack:** C (autotools build), libconfuse (conffile), sqlite3, libevent, Vue 3 (web UI).

## Global Constraints

- Default behavior must be unchanged for every existing config that doesn't set `channels` (default value: `both`).
- Match existing code style exactly — this is an existing C codebase, not greenfield.
- No drive-by refactors outside what each task touches.
- In scope: ALSA, Pulse, FIFO, RAOP, AirPlay backends, core config/API/DB plumbing, and the web UI. Chromecast and streaming (HTTP MP3) backends are explicitly **out of scope** (see Task 1 rationale) — flag this in code as a known limitation, don't attempt to implement it.
- This cannot be verified end-to-end in this session — the user can only confirm correct L/R splitting on real hardware (two physical AirPlay speakers). Do not claim the feature "works" until that manual verification happens (see Task 12).

---

### Task 1: Core channel transform function + enum

**Files:**
- Modify: `src/misc.h:299-325` (Media quality section)
- Modify: `src/misc.c:1949-1990` (Media quality section)
- Create: `src/misc_channels_test.c` (standalone manual test, following the precedent of `src/inputs/librespot-c/tests/test1.c` — this codebase has no unit test harness wired into the autotools build for `src/*.c` logic)

**Interfaces:**
- Produces: `enum output_channels { OUTPUT_CHANNELS_BOTH = 0, OUTPUT_CHANNELS_LEFT, OUTPUT_CHANNELS_RIGHT };`, `enum output_channels output_channels_from_string(const char *s);`, `const char * output_channels_to_string(enum output_channels channels);`, `void channel_transform(uint8_t *buffer, size_t bufsize, int bits_per_sample, int channels, enum output_channels mode);` — all four consumed by every later task.

- [ ] **Step 1: Write the standalone test program**

```c
// src/misc_channels_test.c
// Manual test: gcc -I src -o /tmp/misc_channels_test src/misc_channels_test.c src/misc.c $(pkg-config --cflags --libs libconfuse) -lpthread
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "misc.h"

static void
test_from_to_string(void)
{
  assert(output_channels_from_string("both") == OUTPUT_CHANNELS_BOTH);
  assert(output_channels_from_string("left") == OUTPUT_CHANNELS_LEFT);
  assert(output_channels_from_string("right") == OUTPUT_CHANNELS_RIGHT);
  assert(output_channels_from_string("bogus") == OUTPUT_CHANNELS_BOTH);
  assert(output_channels_from_string(NULL) == OUTPUT_CHANNELS_BOTH);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_BOTH), "both") == 0);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_LEFT), "left") == 0);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_RIGHT), "right") == 0);
  printf("test_from_to_string: OK\n");
}

static void
test_transform_both_is_noop(void)
{
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }; // 4 stereo frames
  int16_t orig[8];
  memcpy(orig, buf, sizeof(buf));

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_BOTH);

  assert(memcmp(buf, orig, sizeof(buf)) == 0);
  printf("test_transform_both_is_noop: OK\n");
}

static void
test_transform_left(void)
{
  // frames: (L,R) = (1,2), (3,4), (5,6), (7,8)
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  int16_t expected[8] = { 1, 1, 3, 3, 5, 5, 7, 7 }; // right := left

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_LEFT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_left: OK\n");
}

static void
test_transform_right(void)
{
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  int16_t expected[8] = { 2, 2, 4, 4, 6, 6, 8, 8 }; // left := right

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_RIGHT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_right: OK\n");
}

static void
test_transform_24bit(void)
{
  // 2 frames, 24-bit stereo, 3 bytes/sample, little-endian: L=0x010203, R=0x040506
  uint8_t buf[12] = {
    0x03, 0x02, 0x01,  0x06, 0x05, 0x04,
    0x03, 0x02, 0x01,  0x06, 0x05, 0x04,
  };
  uint8_t expected[12] = {
    0x03, 0x02, 0x01,  0x03, 0x02, 0x01, // right := left
    0x03, 0x02, 0x01,  0x03, 0x02, 0x01,
  };

  channel_transform(buf, sizeof(buf), 24, 2, OUTPUT_CHANNELS_LEFT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_24bit: OK\n");
}

int
main(void)
{
  test_from_to_string();
  test_transform_both_is_noop();
  test_transform_left();
  test_transform_right();
  test_transform_24bit();
  printf("All misc_channels tests passed.\n");
  return 0;
}
```

- [ ] **Step 2: Compile and run it to verify it fails (functions don't exist yet)**

Run: `gcc -I src -o /tmp/misc_channels_test src/misc_channels_test.c src/misc.c $(pkg-config --cflags --libs libconfuse) -lpthread 2>&1 | head -20`
Expected: compile error — `output_channels_from_string`/`channel_transform` undeclared.

- [ ] **Step 3: Add the enum and declarations to misc.h**

Insert into `src/misc.h`, right after the `struct media_quality` block (after line 322, before `bool quality_is_equal(...)` at line 324-325):

```c
// Which channel(s) an output should play. Default is BOTH so existing
// configs that don't set "channels" keep today's behavior unchanged.
enum output_channels {
  OUTPUT_CHANNELS_BOTH = 0,
  OUTPUT_CHANNELS_LEFT,
  OUTPUT_CHANNELS_RIGHT,
};

enum output_channels
output_channels_from_string(const char *s);

const char *
output_channels_to_string(enum output_channels channels);

// Rewrites an interleaved PCM buffer in place so that, for a stereo split
// mode, both channels carry the same content (left-only: right := left;
// right-only: left := right). No-op for OUTPUT_CHANNELS_BOTH or channels != 2.
// Small and allocation-free by design: called once per output per player tick.
void
channel_transform(uint8_t *buffer, size_t bufsize, int bits_per_sample, int channels, enum output_channels mode);
```

- [ ] **Step 4: Implement in misc.c**

Insert into `src/misc.c`, right after `quality_is_equal()` (after line 1955, before `media_format_from_string` at line 1957):

```c
enum output_channels
output_channels_from_string(const char *s)
{
  if (!s)
    return OUTPUT_CHANNELS_BOTH;
  if (strcmp(s, "left") == 0)
    return OUTPUT_CHANNELS_LEFT;
  if (strcmp(s, "right") == 0)
    return OUTPUT_CHANNELS_RIGHT;

  return OUTPUT_CHANNELS_BOTH;
}

const char *
output_channels_to_string(enum output_channels channels)
{
  if (channels == OUTPUT_CHANNELS_LEFT)
    return "left";
  if (channels == OUTPUT_CHANNELS_RIGHT)
    return "right";

  return "both";
}

void
channel_transform(uint8_t *buffer, size_t bufsize, int bits_per_sample, int channels, enum output_channels mode)
{
  int bytes_per_sample;
  int frame_size;
  size_t nframes;
  size_t i;
  uint8_t *frame;

  if (mode == OUTPUT_CHANNELS_BOTH || channels != 2)
    return;

  bytes_per_sample = bits_per_sample / 8;
  frame_size = bytes_per_sample * channels;
  nframes = bufsize / frame_size;

  for (i = 0, frame = buffer; i < nframes; i++, frame += frame_size)
    {
      if (mode == OUTPUT_CHANNELS_LEFT)
	memcpy(frame + bytes_per_sample, frame, bytes_per_sample); // right := left
      else // OUTPUT_CHANNELS_RIGHT
	memcpy(frame, frame + bytes_per_sample, bytes_per_sample); // left := right
    }
}
```

- [ ] **Step 5: Compile and run the test to verify it passes**

Run: `gcc -I src -o /tmp/misc_channels_test src/misc_channels_test.c src/misc.c $(pkg-config --cflags --libs libconfuse) -lpthread && /tmp/misc_channels_test`
Expected: `All misc_channels tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/misc.h src/misc.c src/misc_channels_test.c
git commit -m "Add channel_transform core helper and output_channels enum"
```

---

### Task 2: `output_device` field + conffile config option

**Files:**
- Modify: `src/outputs.h:120-139` (struct output_device)
- Modify: `src/conffile.c:139-192` (sec_audio, sec_alsa, sec_airplay)

**Interfaces:**
- Consumes: `enum output_channels`, `output_channels_from_string()` from Task 1.
- Produces: `device->channels` field on `struct output_device`, readable by every backend task (6, 7, 8, 9, 10) and by `device_to_speaker_info()` (Task 4).

- [ ] **Step 1: Add the field to `struct output_device`**

In `src/outputs.h`, right after `int offset_ms;` (line 138):

```c
  // For user config of per-output channel selection (both/left/right)
  enum output_channels channels;
```

(No test here — this is a plain struct field; it's exercised by Tasks 4/6-10's tests.)

- [ ] **Step 2: Add the `channels` config option to `sec_audio`, `sec_alsa`, and `sec_airplay`**

In `src/conffile.c`, `sec_audio[]` (after line 146, `CFG_STR("mixer_device", ...)`):

```c
    CFG_STR("channels", "both", CFGF_NONE),
```

In `sec_alsa[]` (after line 161, `CFG_STR("mixer_device", ...)`):

```c
    CFG_STR("channels", "both", CFGF_NONE),
```

In `sec_airplay[]` (after line 187, `CFG_STR("nickname", ...)`):

```c
    CFG_STR("channels", "both", CFGF_NONE),
```

- [ ] **Step 3: Verify it builds**

Run: `autoreconf -vi && ./configure --prefix=/usr --enable-chromecast --with-pulseaudio && make -C src conffile.o outputs.lo`
Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add src/outputs.h src/conffile.c
git commit -m "Add channels field to output_device and channels conffile option"
```

---

### Task 3: DB schema migration + persistence

**Files:**
- Modify: `src/db_init.h:28-29` (SCHEMA_VERSION_MINOR)
- Modify: `src/db_init.c:149-158` (T_SPEAKERS)
- Modify: `src/db_upgrade.c` (new migration block + switch wiring)
- Modify: `src/db.c:4792-4852` (db_speaker_save, db_speaker_get)

**Interfaces:**
- Consumes: `device->channels` (Task 2), `output_channels_from_string`/`output_channels_to_string` (Task 1).
- Produces: `channels` column on the `speakers` table, persisted/restored on `db_speaker_save()`/`db_speaker_get()` — consumed by `outputs_device_add()`/`outputs_device_remove()` in `outputs.c` (already call these, unchanged).

- [ ] **Step 1: Bump schema version**

In `src/db_init.h:29`, change:
```c
#define SCHEMA_VERSION_MINOR 3
```
to:
```c
#define SCHEMA_VERSION_MINOR 4
```

- [ ] **Step 2: Add the column to the CREATE TABLE (fresh installs)**

In `src/db_init.c`, `T_SPEAKERS` (line 149-158), after `"   offset_ms      INTEGER DEFAULT 0"`:

```c
#define T_SPEAKERS					\
  "CREATE TABLE IF NOT EXISTS speakers("		\
  "   id             INTEGER PRIMARY KEY NOT NULL,"	\
  "   selected       INTEGER NOT NULL,"			\
  "   volume         INTEGER NOT NULL,"			\
  "   name           VARCHAR(255) DEFAULT NULL,"       \
  "   auth_key       VARCHAR(2048) DEFAULT NULL,"      \
  "   format         INTEGER DEFAULT 0,"                \
  "   offset_ms      INTEGER DEFAULT 0,"			\
  "   channels       INTEGER DEFAULT 0"			\
  ");"
```
(`0` == `OUTPUT_CHANNELS_BOTH`, matching Task 1's enum — this is what keeps existing/fresh DBs defaulting to current behavior.)

- [ ] **Step 3: Add the migration for existing DBs**

In `src/db_upgrade.c`, right after the `U_v2203_*` block (after line 1284, before `/* -------------------------- Main upgrade handler -------------------------- */`):

```c
/* ---------------------------- 22.03 -> 22.04 ------------------------------ */

#define U_v2204_ALTER_SPEAKERS_ADD_CHANNELS \
  "ALTER TABLE speakers ADD COLUMN channels INTEGER DEFAULT 0;"

#define U_v2204_SCVER_MAJOR                    \
  "UPDATE admin SET value = '22' WHERE key = 'schema_version_major';"
#define U_v2204_SCVER_MINOR                    \
  "UPDATE admin SET value = '04' WHERE key = 'schema_version_minor';"

static const struct db_upgrade_query db_upgrade_v2204_queries[] =
  {
    { U_v2204_ALTER_SPEAKERS_ADD_CHANNELS, "alter table speakers add column channels" },

    { U_v2204_SCVER_MAJOR,    "set schema_version_major to 22" },
    { U_v2204_SCVER_MINOR,    "set schema_version_minor to 04" },
  };
```

- [ ] **Step 4: Wire the new migration into the upgrade switch**

In `src/db_upgrade.c`, in `db_upgrade()`, change the existing terminal case:
```c
    case 2202:
      ret = db_generic_upgrade(hdl, db_upgrade_v2203_queries, ARRAY_SIZE(db_upgrade_v2203_queries));
      if (ret < 0)
	return -1;

      /* Last case statement is the only one that ends with a break statement! */
      break;
```
to:
```c
    case 2202:
      ret = db_generic_upgrade(hdl, db_upgrade_v2203_queries, ARRAY_SIZE(db_upgrade_v2203_queries));
      if (ret < 0)
	return -1;

      /* FALLTHROUGH */

    case 2203:
      ret = db_generic_upgrade(hdl, db_upgrade_v2204_queries, ARRAY_SIZE(db_upgrade_v2204_queries));
      if (ret < 0)
	return -1;

      /* Last case statement is the only one that ends with a break statement! */
      break;
```

- [ ] **Step 5: Persist/restore the field in db.c**

In `src/db.c`, `db_speaker_save()` (lines 4794-4803):
```c
int
db_speaker_save(struct output_device *device)
{
#define Q_TMPL "INSERT OR REPLACE INTO speakers (id, selected, volume, name, auth_key, format, offset_ms, channels) VALUES (%" PRIi64 ", %d, %d, %Q, %Q, %d, %d, %d);"
  char *query;

  query = sqlite3_mprintf(Q_TMPL, device->id, device->selected, device->volume, device->name, device->auth_key, device->selected_format, device->offset_ms, device->channels);

  return db_query_run(query, 1, 0);
#undef Q_TMPL
}
```

In `db_speaker_get()` (lines 4805-4852), change the query and add a column read:
```c
#define Q_TMPL "SELECT s.selected, s.volume, s.name, s.auth_key, s.format, s.offset_ms, s.channels FROM speakers s WHERE s.id = %" PRIi64 ";"
```
and after `device->offset_ms = sqlite3_column_int(stmt, 5);` (line 4851):
```c
  device->channels = sqlite3_column_int(stmt, 6);
```

- [ ] **Step 6: Verify it builds and a fresh DB migrates cleanly**

Run:
```bash
make -C src db.o db_init.o db_upgrade.o
rm -f /tmp/test_owntone.db
sqlite3 /tmp/test_owntone.db "CREATE TABLE speakers (id INTEGER PRIMARY KEY NOT NULL, selected INTEGER NOT NULL, volume INTEGER NOT NULL, name VARCHAR(255) DEFAULT NULL, auth_key VARCHAR(2048) DEFAULT NULL, format INTEGER DEFAULT 0, offset_ms INTEGER DEFAULT 0);"
sqlite3 /tmp/test_owntone.db "CREATE TABLE admin (key VARCHAR(32) PRIMARY KEY NOT NULL, value VARCHAR(32) NOT NULL); INSERT INTO admin VALUES ('schema_version_major','22'); INSERT INTO admin VALUES ('schema_version_minor','03');"
sqlite3 /tmp/test_owntone.db "ALTER TABLE speakers ADD COLUMN channels INTEGER DEFAULT 0;" # simulates db_upgrade's effect
sqlite3 /tmp/test_owntone.db ".schema speakers"
```
Expected: `channels` column present with `DEFAULT 0`.

- [ ] **Step 7: Commit**

```bash
git add src/db_init.h src/db_init.c src/db_upgrade.c src/db.c
git commit -m "Persist per-speaker channel selection in the database"
```

---

### Task 4: player.c setter/getter + JSON serialization struct

**Files:**
- Modify: `src/player.h:31-53` (struct player_speaker_info), `src/player.h:99-137` (declarations)
- Modify: `src/player.c:145-163` (struct speaker_attr_param), `src/player.c:2541-2572` (device_to_speaker_info), `src/player.c:2897-2955` (speaker_format_set/speaker_offset_ms_set — add speaker_channels_set alongside), `src/player.c:3654-3673` (player_speaker_format_set/player_speaker_offset_ms_set — add player_speaker_channels_set alongside)

**Interfaces:**
- Consumes: `device->channels`, `enum output_channels`, `output_channels_to_string()` (Tasks 1-2).
- Produces: `int player_speaker_channels_set(uint64_t id, enum output_channels channels);` and `spk->channels` on `struct player_speaker_info` — consumed by Task 5 (httpd_jsonapi.c) and Task 11 (nothing web-side calls player.c directly, but this is the source of truth GET/PUT reads from).

- [ ] **Step 1: Add `channels` to `struct player_speaker_info`**

In `src/player.h`, after `int offset_ms;` (line ~38, inside the struct shown at lines 31-53):

```c
  enum output_channels channels;
```

- [ ] **Step 2: Add `channels` to `struct speaker_attr_param`**

In `src/player.c:145-163`, after `int offset_ms;` (line 157):

```c
  enum output_channels channels;
```

- [ ] **Step 3: Fill it in `device_to_speaker_info()`**

In `src/player.c`, after `spk->offset_ms = device->offset_ms;` (line 2553):

```c
  spk->channels = device->channels;
```

- [ ] **Step 4: Add the `speaker_channels_set` command handler**

In `src/player.c`, right after `speaker_offset_ms_set()` (after line 2954, mirroring its shape but without the offset-range validation since channels is a closed enum, always valid):

```c
static enum command_state
speaker_channels_set(void *arg, int *retval)
{
  struct speaker_attr_param *param = arg;
  struct output_device *device;

  device = outputs_device_get(param->spk_id);
  if (!device)
    {
      DPRINTF(E_LOG, L_PLAYER, "Error setting channels, device %" PRIu64 " unknown\n", param->spk_id);
      *retval = -1;
      return COMMAND_END;
    }

  device->channels = param->channels;

  // Same rationale as speaker_offset_ms_set: can't change mid-playback, but
  // if paused we stop the session so the new mode takes effect on restart
  if (player_state == PLAY_PAUSED)
    *retval = outputs_device_stop(device, device_shutdown_cb);
  else
    *retval = 0;

  if (*retval > 0)
    return COMMAND_PENDING; // async

  return COMMAND_END;
}
```

- [ ] **Step 5: Add the public `player_speaker_channels_set()` wrapper**

In `src/player.c`, right after `player_speaker_offset_ms_set()` (after line 3673):

```c
int
player_speaker_channels_set(uint64_t id, enum output_channels channels)
{
  struct speaker_attr_param param;

  param.spk_id = id;
  param.channels = channels;

  return commands_exec_sync(cmdbase, speaker_channels_set, speaker_generic_bh, &param);
}
```

- [ ] **Step 6: Declare it in player.h**

In `src/player.h`, right after `player_speaker_offset_ms_set` (line 136-137):

```c
int
player_speaker_channels_set(uint64_t id, enum output_channels channels);
```

- [ ] **Step 7: Verify it builds**

Run: `make -C src player.o`
Expected: no errors.

- [ ] **Step 8: Commit**

```bash
git add src/player.h src/player.c
git commit -m "Add player_speaker_channels_set and thread channels through speaker info"
```

---

### Task 5: JSON API — GET/PUT `/api/outputs`

**Files:**
- Modify: `src/httpd_jsonapi.c:1610-1641` (speaker_to_json), `src/httpd_jsonapi.c:1694-1767` (jsonapi_reply_outputs_put_byid)

**Interfaces:**
- Consumes: `spk->channels`, `player_speaker_channels_set()`, `output_channels_to_string()`/`output_channels_from_string()` (Task 4, Task 1).
- Produces: `"channels"` field in the output JSON objects, consumed by the web UI (Task 11).

- [ ] **Step 1: Add the field to serialization**

In `src/httpd_jsonapi.c`, `speaker_to_json()`, right after the existing `"offset_ms"` line:

```c
  json_object_object_add(output, "channels", json_object_new_string(output_channels_to_string(spk->channels)));
```

- [ ] **Step 2: Accept it in the PUT handler**

In `src/httpd_jsonapi.c`, `jsonapi_reply_outputs_put_byid()`, add a block mirroring the existing `offset_ms` block:

```c
  if (jparse_contains_key(request, "channels", json_type_string))
    {
      const char *channels_str = jparse_str_from_obj(request, "channels");
      ret = player_speaker_channels_set(output_id, output_channels_from_string(channels_str));
      if (ret < 0)
	goto error;
    }
```

(Match the exact local-variable/`goto error` idiom already used by the neighboring `offset_ms`/`format` blocks in this function — read the surrounding code first since the exact variable names/error label may differ slightly; keep the new block stylistically identical to its neighbors.)

- [ ] **Step 3: Manual smoke test against a running instance**

Run:
```bash
sudo make install
sudo /usr/sbin/owntone -f -t &
sleep 2
curl -s http://localhost:3689/api/outputs | python3 -m json.tool | grep -A1 channels
curl -X PUT http://localhost:3689/api/outputs/<id> -H 'Content-Type: application/json' -d '{"channels":"left"}'
curl -s http://localhost:3689/api/outputs | python3 -m json.tool | grep -A1 channels
```
Expected: `"channels": "both"` initially, `"channels": "left"` after the PUT.

- [ ] **Step 4: Commit**

```bash
git add src/httpd_jsonapi.c
git commit -m "Expose channels in the outputs JSON API"
```

---

### Task 6: ALSA backend integration

**Files:**
- Modify: `src/outputs/alsa.c:132-151` (struct alsa_session), `src/outputs/alsa.c:1145-1195` (alsa_session_make), `src/outputs/alsa.c:1360-1409` (alsa_device_add), `src/outputs/alsa.c:803-867` (buffer_write)

**Interfaces:**
- Consumes: `device->channels`, `channel_transform()`, `enum output_channels` (Tasks 1-2).
- Produces: nothing consumed by later tasks — ALSA is a leaf backend.

- [ ] **Step 1: Read `channels` from config in `alsa_device_add()`**

In `src/outputs/alsa.c`, right after the `offset_ms` block (after line 1402):

```c
  device->channels = output_channels_from_string(cfg_getstr(cfg_audio, "channels"));
```

- [ ] **Step 2: Copy it onto the session in `alsa_session_make()`**

In `struct alsa_session` (line 132-151), add after `uint64_t delay_ms;` (line 145):

```c
  enum output_channels channels;
```

In `alsa_session_make()`, after `as->delay_ms += device->offset_ms;` (line 1174-ish):

```c
  as->channels = device->channels;
```

- [ ] **Step 3: Apply the transform at the buffer-copy point**

In `buffer_write()` (`src/outputs/alsa.c:803-867`), the function signature is `buffer_write(struct alsa_playback_session *pb, struct output_data *odata, snd_pcm_sframes_t avail)` — `pb` doesn't carry `channels` today (it's the per-quality sub-session; `channels` lives one level up on `struct alsa_session`). Since `odata->buffer` is the *shared* buffer (read by every ALSA session at that quality), it must not be mutated in place before checking whether other sessions also read it this tick — but ALSA's `playback_write()` (line 1038) is called once per `pb` with its own `avail`, and each `pb` belongs to exactly one `as`. Add a small scratch buffer on `struct alsa_playback_session` (find its definition near line ~100-130, alongside `pcm`/`prebuf`) sized to the max per-tick `bufsize`, copy-then-transform into it, and pass the scratch buffer to `ringbuffer_write`/`snd_pcm_writei` instead of `odata->buffer` directly:

```c
static int
buffer_write(struct alsa_playback_session *pb, struct output_data *odata, snd_pcm_sframes_t avail)
{
  uint8_t *buf = odata->buffer;
  size_t bufsize = odata->bufsize;
  snd_pcm_sframes_t nsamp = odata->samples;
  ...

  if (pb->as->channels != OUTPUT_CHANNELS_BOTH)
    {
      if (pb->chanbuf_size < bufsize)
	{
	  pb->chanbuf = realloc(pb->chanbuf, bufsize);
	  pb->chanbuf_size = bufsize;
	}
      memcpy(pb->chanbuf, buf, bufsize);
      channel_transform(pb->chanbuf, bufsize, odata->quality.bits_per_sample, odata->quality.channels, pb->as->channels);
      buf = pb->chanbuf;
    }

  // ... existing body, replacing every remaining use of odata->buffer with buf
}
```
Add `struct alsa_session *as;` (back-pointer), `uint8_t *chanbuf;`, `size_t chanbuf_size;` to `struct alsa_playback_session`, set `pb->as = as;` wherever a `struct alsa_playback_session` is allocated (in `playback_session_add()`), and free `pb->chanbuf` in whatever function frees the playback session (mirror how `pb->prebuf`/`pb->pcm` are torn down).

(This step touches more of `buffer_write()`'s body than shown here — read the full current function before editing, since every `odata->buffer`/`odata->bufsize` reference inside it must consistently use the (possibly transformed) local `buf`/`bufsize`, not the original `odata` fields, once `channels != BOTH`.)

- [ ] **Step 4: Verify it builds**

Run: `make -C src/outputs alsa.o` (or `make` from repo root if `alsa.o` isn't a valid target path — check `src/outputs/Makefile.am`/`src/Makefile.am` for the right target name first)
Expected: no errors.

- [ ] **Step 5: Manual test with `arecord`/loopback** (best available verification without real hardware)

```bash
# Configure an ALSA loopback device (snd-aloop) with channels = "left" in owntone.conf,
# play known stereo test audio through OwnTone, and capture:
arecord -D hw:Loopback,1 -f S16_LE -c 2 -r 44100 -d 5 /tmp/capture.wav
# Inspect: left and right channel samples should be identical.
```

- [ ] **Step 6: Commit**

```bash
git add src/outputs/alsa.c
git commit -m "Apply per-output channel transform in the ALSA backend"
```

---

### Task 7: Pulse backend integration

**Files:**
- Modify: `src/outputs/pulse.c:59-79` (struct pulse_session), `src/outputs/pulse.c:151-184` (pulse_session_make), `src/outputs/pulse.c:400-436` (pulse device-add path, near existing `offset_ms`/`nickname` reads), `src/outputs/pulse.c:692-727` (playback_write)

**Interfaces:**
- Consumes: `device->channels`, `channel_transform()` (Tasks 1-2).

- [ ] **Step 1: Read `channels` from config**

In `src/outputs/pulse.c`, wherever `device->offset_ms = offset_ms;` is set (line ~432, in the device-add function), add immediately after:

```c
  device->channels = output_channels_from_string(cfg_getstr(cfg_getsec(cfg, "audio"), "channels"));
```
(Use the same `cfg_getsec(cfg, "audio")` call already used two lines above it for `nickname` — read the surrounding function first to match the exact `cfg_t *` variable already in scope instead of re-fetching it.)

- [ ] **Step 2: Copy it onto the session**

In `struct pulse_session` (line 59-79), add after `struct media_quality quality;` (line 72):

```c
  enum output_channels channels;
  uint8_t *chanbuf;
  size_t chanbuf_size;
```

In `pulse_session_make()`, after `ps->volume = pulse_from_device_volume(device->volume);` (line 169):

```c
  ps->channels = device->channels;
```

- [ ] **Step 3: Apply the transform before `pa_stream_write()`**

In `playback_write()` (`src/outputs/pulse.c:692-727`), replace the direct pass-through:

```c
static void
playback_write(struct pulse_session *ps, struct output_buffer *obuf)
{
  int i;
  int ret;
  uint8_t *buf;

  for (i = 0; obuf->data[i].buffer; i++)
    {
      if (quality_is_equal(&ps->quality, &obuf->data[i].quality))
	break;
    }

  if (!obuf->data[i].buffer)
    {
      DPRINTF(E_LOG, L_LAUDIO, "Output not delivering required data quality, aborting\n");
      return;
    }

  buf = obuf->data[i].buffer;
  if (ps->channels != OUTPUT_CHANNELS_BOTH)
    {
      if (ps->chanbuf_size < obuf->data[i].bufsize)
	{
	  ps->chanbuf = realloc(ps->chanbuf, obuf->data[i].bufsize);
	  ps->chanbuf_size = obuf->data[i].bufsize;
	}
      memcpy(ps->chanbuf, obuf->data[i].buffer, obuf->data[i].bufsize);
      channel_transform(ps->chanbuf, obuf->data[i].bufsize, obuf->data[i].quality.bits_per_sample, obuf->data[i].quality.channels, ps->channels);
      buf = ps->chanbuf;
    }

  pa_threaded_mainloop_lock(pulse.mainloop);

  ret = pa_stream_write(ps->stream, buf, obuf->data[i].bufsize, NULL, 0LL, PA_SEEK_RELATIVE);
  if (ret < 0)
    {
      // ... keep existing error handling unchanged
    }

 unlock:
  pa_threaded_mainloop_unlock(pulse.mainloop);
}
```
(Read the full current function first — the exact error-handling block and `goto unlock` structure must be preserved verbatim; only the `buf`/transform lines and the `pa_stream_write` argument are new.)

Also free `ps->chanbuf` wherever `struct pulse_session` is torn down (mirror `ps->devname`/`ps->stream` cleanup).

- [ ] **Step 4: Verify it builds**

Run: `make -C src` (with `--with-pulseaudio` configured)
Expected: no errors.

- [ ] **Step 5: Commit**

```bash
git add src/outputs/pulse.c
git commit -m "Apply per-output channel transform in the Pulse backend"
```

---

### Task 8: FIFO backend integration

**Files:**
- Modify: `src/outputs/fifo.c:85-100` (struct fifo_session), `src/outputs/fifo.c:250-281` (fifo_session_make), `src/outputs/fifo.c:490-510` (fifo device-add, near existing `nickname` read), `src/outputs/fifo.c:402-444` (fifo_write)

**Interfaces:**
- Consumes: `device->channels`, `channel_transform()` (Tasks 1-2).

- [ ] **Step 1: Read `channels` from config**

In `src/outputs/fifo.c`, near where `nickname = cfg_getstr(cfg_fifo, "nickname");` is read (line 499), read and store on the device struct being built at that point:

```c
  device->channels = output_channels_from_string(cfg_getstr(cfg_fifo, "channels"));
```
(Confirm the exact local variable name for the `struct output_device *` in scope at that point before writing this line — read the full function first.)

- [ ] **Step 2: Copy it onto the session**

In `struct fifo_session` (line 85-100), add after `uint64_t device_id;` / `int callback_id;` (line 98-99):

```c
  enum output_channels channels;
```

In `fifo_session_make()`, after `fifo_session->callback_id = callback_id;` (line 260):

```c
  fifo_session->channels = device->channels;
```

- [ ] **Step 3: Apply the transform right after the existing `memcpy`**

In `fifo_write()` (`src/outputs/fifo.c:402-444`), right after the existing copy (line 428):

```c
  memcpy(packet->samples, obuf->data[i].buffer, obuf->data[i].bufsize);
  packet->samples_size = obuf->data[i].bufsize;
  channel_transform(packet->samples, packet->samples_size, obuf->data[i].quality.bits_per_sample, obuf->data[i].quality.channels, fifo_session->channels);
  packet->pts = obuf->pts;
```

(Since `packet->samples` is already a fresh per-write `malloc`, this is the simplest of all five backends — no scratch buffer needed, transform straight in place.)

- [ ] **Step 4: Verify it builds**

Run: `make -C src/outputs fifo.o` (check exact target/path from `Makefile.am` first)
Expected: no errors.

- [ ] **Step 5: Manual test**

```bash
mkfifo /tmp/test.fifo
# configure a fifo {} output with path=/tmp/test.fifo and channels="left"
cat /tmp/test.fifo | ffprobe -f s16le -ar 44100 -ac 2 -i - -af "astats=metadata=1" -f null - 2>&1 | grep -i "channel"
```
Expected: left and right channel stats identical while a known stereo track plays.

- [ ] **Step 6: Commit**

```bash
git add src/outputs/fifo.c
git commit -m "Apply per-output channel transform in the FIFO backend"
```

---

### Task 9: RAOP backend integration (master-session repartition)

**Files:**
- Modify: `src/outputs/raop.c:159-185` (struct raop_master_session), `src/outputs/raop.c:1866-1930` (master_session_make), `src/outputs/raop.c:2188-2283` (session_make), `src/outputs/raop.c:4200-4280` (raop device-add, near existing `nickname` read), `src/outputs/raop.c:4588-4638` (raop_write)

**Interfaces:**
- Consumes: `device->channels`, `channel_transform()` (Tasks 1-2).
- Produces: extended `master_session_make(quality, encrypt, channels)` signature — Task 10 (AirPlay) mirrors this shape independently in its own file, no direct code sharing.

**Why this task is bigger than Tasks 6-8:** RAOP shares one ALAC-encoded stream (one `raop_master_session`) across every device requesting the same quality, keyed today by `(quality, encrypt)`. Two same-quality AirPlay speakers — your actual stereo-pair case — would otherwise land on the *same* master session and get identical audio no matter what `channels` is set to. The fix: make the master-session cache key `(quality, encrypt, channels)`, so a "left" device and a "right" device at the same quality get two distinct master sessions, each independently encoding its own already-transformed PCM.

- [ ] **Step 1: Read `channels` from config**

In `src/outputs/raop.c`'s device-add function (`raop.c:4200-4280`, the one already reading `devcfg = cfg_gettsec(cfg, "airplay", device_name);` at line 4239), add after `rd->supported_formats = MEDIA_FORMAT_ALAC;` (line 4277):

```c
  rd->channels = output_channels_from_string(devcfg ? cfg_getstr(devcfg, "channels") : NULL);
```

- [ ] **Step 2: Add `channels` to `struct raop_master_session` and extend `master_session_make()`'s cache key**

In `struct raop_master_session` (line 159-185), add after `bool encrypt;` (line 175):

```c
  enum output_channels channels;
  uint8_t *chanbuf;
  size_t chanbuf_size;
```

In `master_session_make()` (line 1866-1930), change the signature and the matching loop:

```c
static struct raop_master_session *
master_session_make(struct media_quality *quality, bool encrypt, enum output_channels channels)
{
  struct raop_master_session *rms;
  ...

  // First check if we already have a suitable session
  for (rms = raop_master_sessions; rms; rms = rms->next)
    {
      if (encrypt == rms->encrypt && channels == rms->channels && quality_is_equal(quality, &rms->rtp_session->quality))
	return rms;
    }

  // ... existing creation body unchanged until the final field assignments ...

  rms->encrypt = encrypt;
  rms->channels = channels;
  rms->quality = *quality;
  ...
}
```

- [ ] **Step 3: Pass `channels` through from `session_make()`**

In `session_make()` (line 2188-2283), change:
```c
  rs->master_session = master_session_make(&rd->quality, rs->encrypt);
```
to:
```c
  rs->master_session = master_session_make(&rd->quality, rs->encrypt, rd->channels);
```

- [ ] **Step 4: Apply the transform before the shared encode step**

In `raop_write()` (line 4588-4638), the copy point is the `evbuffer_add(rms->input_buffer, obuf->data[i].buffer, obuf->data[i].bufsize);` marked `// TODO avoid this copy` (around line 4610). Since `obuf->data[i].buffer` is shared across every `rms` at that quality (including ones with a different `channels` mode), transform into a per-`rms` scratch buffer first, then add *that* to the evbuffer:

```c
	  timestamp_set(rms, obuf->pts);
	  packets_sync_send(rms);

	  if (rms->channels == OUTPUT_CHANNELS_BOTH)
	    {
	      evbuffer_add(rms->input_buffer, obuf->data[i].buffer, obuf->data[i].bufsize);
	    }
	  else
	    {
	      if (rms->chanbuf_size < obuf->data[i].bufsize)
		{
		  rms->chanbuf = realloc(rms->chanbuf, obuf->data[i].bufsize);
		  rms->chanbuf_size = obuf->data[i].bufsize;
		}
	      memcpy(rms->chanbuf, obuf->data[i].buffer, obuf->data[i].bufsize);
	      channel_transform(rms->chanbuf, obuf->data[i].bufsize, obuf->data[i].quality.bits_per_sample, obuf->data[i].quality.channels, rms->channels);
	      evbuffer_add(rms->input_buffer, rms->chanbuf, obuf->data[i].bufsize);
	    }

	  rms->input_buffer_samples += obuf->data[i].samples;
```

Also free `rms->chanbuf` in `master_session_free()` (wherever `rms->input_buffer`/`rms->rawbuf` are freed today).

- [ ] **Step 5: Verify it builds**

Run: `make -C src/outputs raop.o` (check exact target path first)
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add src/outputs/raop.c
git commit -m "Apply per-output channel transform in the RAOP backend, partition master sessions by channel mode"
```

---

### Task 10: AirPlay backend integration (master-session repartition, mirrors Task 9)

**Files:**
- Modify: `src/outputs/airplay.c:211-238` (struct airplay_master_session), `src/outputs/airplay.c:1780-1850` (master_session_make — confirm exact line range when editing), `src/outputs/airplay.c:1584-1647` (session_make), `src/outputs/airplay.c:3960-4010` (device-add, near existing `nickname` read), `src/outputs/airplay.c:4233-4283` (airplay_write)

**Interfaces:**
- Consumes: `device->channels`, `channel_transform()` (Tasks 1-2). Structurally identical to Task 9 — same rationale, same shape, different file/struct names (`airplay_master_session`/`airplay_session` instead of `raop_*`).

- [ ] **Step 1: Read `channels` from config**

In `src/outputs/airplay.c`'s device-add function (`airplay.c:3960-4010`), add after `device->supported_formats = ...;` (near line 4009-4010, wherever that field is set — confirm exact line first):

```c
  device->channels = output_channels_from_string(devcfg ? cfg_getstr(devcfg, "channels") : NULL);
```

- [ ] **Step 2: Add `channels` to `struct airplay_master_session` and extend its `master_session_make()`'s cache key**

Same shape as Task 9 Step 2, applied to `struct airplay_master_session` (line 211-238) and its own `master_session_make()` (find its definition — likely near line 1780-1850, search for `airplay_master_sessions` linked-list walk):

```c
  enum output_channels channels;
  uint8_t *chanbuf;
  size_t chanbuf_size;
```
and the same `channels == ams->channels` addition to the existing-session lookup loop, plus `ams->channels = channels;` at creation.

- [ ] **Step 3: Pass `channels` through from `session_make()`**

In `session_make()` (line 1584-1647), change:
```c
  session->master_session = master_session_make(&device->quality, extra->use_ptp);
```
to:
```c
  session->master_session = master_session_make(&device->quality, extra->use_ptp, device->channels);
```
(confirm the exact parameter list of `master_session_make` in this file first — it takes `use_ptp` where RAOP's takes `encrypt`; add `channels` as a new trailing parameter the same way.)

- [ ] **Step 4: Apply the transform before the shared encode step**

Same shape as Task 9 Step 4, applied to `airplay_write()` (line 4233-4283) at its `evbuffer_add(ams->input_buffer, obuf->data[i].buffer, obuf->data[i].bufsize);` copy point (marked `// TODO avoid this copy`, around line 4255).

- [ ] **Step 5: Verify it builds**

Run: `make -C src/outputs airplay.o` (check exact target path first)
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add src/outputs/airplay.c
git commit -m "Apply per-output channel transform in the AirPlay backend, partition master sessions by channel mode"
```

---

### Task 11: Web UI control

**Files:**
- Modify: `web-src/src/components/ControlOutputVolume.vue` (full file, 65 lines — shown in investigation above)
- Modify: `web-src/src/api/outputs.js` (no change needed — `update(id, output)` already generic)

**Interfaces:**
- Consumes: `output.channels` (a string: `"both"`/`"left"`/`"right"`) from the `GET /api/outputs` response shape produced by Task 5; calls `outputs.update(id, { channels })` (existing function, Task 5's `PUT` handler consumes it).

- [ ] **Step 1: Add a channels dropdown next to the volume slider**

Replace the full contents of `web-src/src/components/ControlOutputVolume.vue`:

```vue
<template>
  <div class="media is-align-items-center mb-0">
    <div class="media-left">
      <button
        class="button is-small"
        :class="{ 'has-text-grey-light': !output.selected }"
        @click="toggle"
      >
        <mdicon class="icon" :name="icon" :title="output.type" />
      </button>
    </div>
    <div class="media-content">
      <div
        class="is-size-7 is-uppercase"
        :class="{ 'has-text-grey-light': !output.selected }"
        v-text="output.name"
      />
      <control-slider
        v-model:value="volume"
        :disabled="!output.selected"
        :max="100"
        @change="changeVolume"
      />
      <control-dropdown
        v-model:value="channels"
        :options="channelsOptions"
        @update:value="changeChannels"
      />
    </div>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue'

import ControlDropdown from '@/components/ControlDropdown.vue'
import ControlSlider from '@/components/ControlSlider.vue'
import outputs from '@/api/outputs'
import player from '@/api/player'

const props = defineProps({ output: { required: true, type: Object } })

const volume = ref(props.output.volume)
const channels = ref(props.output.channels)

const channelsOptions = [
  { id: 'both', name: 'Both' },
  { id: 'left', name: 'Left' },
  { id: 'right', name: 'Right' }
]

const icon = computed(() => {
  if (props.output.type.startsWith('AirPlay')) {
    return 'cast-variant'
  } else if (props.output.type === 'Chromecast') {
    return 'cast'
  } else if (props.output.type === 'fifo') {
    return 'pipe'
  }
  return 'server'
})

watch(
  () => props.output,
  (newOutput) => {
    volume.value = newOutput.volume
    channels.value = newOutput.channels
  }
)

const changeVolume = () => {
  player.setVolume(volume.value, props.output.id)
}

const changeChannels = (mode) => {
  outputs.update(props.output.id, { channels: mode })
}

const toggle = () => {
  outputs.update(props.output.id, { selected: !props.output.selected })
}
</script>
```

- [ ] **Step 2: Build the web UI and verify no errors**

Run: `cd web-src && npm ci && npm run build`
Expected: build succeeds, no Vue compiler warnings about the new component usage.

- [ ] **Step 3: Manual browser check**

```bash
cd web-src && npm run dev
```
Open the dev server, navigate to the Outputs panel, confirm each output row shows a Both/Left/Right dropdown next to its volume slider, and that changing it fires a `PUT /api/outputs/<id>` with `{"channels": "..."}` (check browser devtools Network tab).

- [ ] **Step 4: Commit**

```bash
git add web-src/src/components/ControlOutputVolume.vue
git commit -m "Add per-output channel selector to the outputs panel"
```

---

### Task 12: Manual hardware verification (blocking — do not skip or claim success without this)

**Files:** none — this is a verification-only task.

- [ ] **Step 1: Configure two AirPlay outputs as a stereo pair**

In `owntone.conf`, add two `airplay "<name>" { channels = "left" }` / `channels = "right"` blocks for the two physical speakers.

- [ ] **Step 2: Restart OwnTone and select both outputs**

```bash
sudo systemctl restart owntone   # or however it's currently deployed
```
Select both speakers in the OwnTone web UI/Remote, confirm both show as selected.

- [ ] **Step 3: Play a track with distinct, known left/right content**

A test tone file with a pure left-channel sweep and pure right-channel sweep (or any commercial track known to have hard-panned content) works well for audibly confirming which physical speaker plays which side.

- [ ] **Step 4: Confirm audibly**

The "left" speaker should audibly play only the left-channel content (as mono, per Task 1's `left := left, right := left` behavior), and the "right" speaker only the right-channel content. Confirm this holds over at least several minutes of continuous playback (checking for any drift or dropout), and after a pause/resume cycle (exercises the `speaker_channels_set` mid-playback-block path from Task 4).

- [ ] **Step 5: Report back**

Only after this step passes should the feature be described as working — per the original constraint, this cannot be verified in an automated/headless session.

---

## Known limitations (explicitly out of scope, flag in a code comment if convenient)

- **Chromecast**: not implemented. `cast.c` uses one global singleton `cast_master_session` shared by every Chromecast device regardless of quality or anything else — supporting per-device channels here would require breaking that singleton into one-per-(quality, channel-mode) session, a materially larger change than Tasks 9/10, and not exercised by the AirPlay stereo-pair use case this plan targets.
- **Streaming (web/HTTP MP3 listeners)**: not implemented. `streaming.c` groups anonymous HTTP listeners by `(format, quality)`, not by discrete per-speaker `output_device` — there's no natural "this specific output" concept to attach a channel mode to without a larger redesign of that grouping key.
