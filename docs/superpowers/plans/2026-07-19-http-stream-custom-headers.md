# Per-Stream Custom HTTP Request Headers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a client queuing a raw `http(s)://` URL (`DATA_KIND_HTTP`) supply optional custom HTTP request headers (e.g. `Referer`) that ffmpeg actually sends when it fetches that stream, so URLs that 403 without a matching header (e.g. YouTube CDN URLs resolved via `yt-dlp -g`) can be played.

**Architecture:** A `headers` string (already-formatted as `\r\n`-joined `Key: Value` pairs, matching ffmpeg's own `headers` AVOption format) is stored as a new column on the queue item, mirroring `artwork_url` — a queue-item-only field with no counterpart in the scanned-library `files` table (`qi_mfi_map` entry uses the same `-1, -1` "no mfi/dbmfi counterpart" sentinel `artwork_url` already uses). It's threaded synchronously through `POST /api/queue/items/add` → the library queue-add dispatch → `struct input_source` → `transcode_decode_setup_args` → `open_input()`'s `av_dict_set(&options, "headers", ...)` — the same call site that already sets `user_agent`/`icy`/`reconnect` for HTTP sources. No new field is needed on `struct decode_ctx`: the string is only needed synchronously inside `open_input()`, so it's passed as a plain parameter, not stored.

**Tech Stack:** C (autotools build), ffmpeg/libav (AVDictionary `headers` AVOption), sqlite3, libcurl (secondary, lower-priority path), json-c (REST API).

## Global Constraints

- Scoped to `DATA_KIND_HTTP` only. Do not touch `DATA_KIND_PIPE` or `INPUT_TYPE_SPOTIFY`/librespot-c code paths — `spotify_webapi.c`'s `queue_item_add` implementation gets a signature-compatibility update only (new parameter, ignored), no behavior change.
- Queuing a plain URL with no `headers` must behave exactly as today — no regression. The new column defaults to `NULL`/unset, and `open_input()`'s `av_dict_set(&options, "headers", ...)` call must be skipped entirely when there's nothing to set (an empty/absent header string must not become a real `""` header line sent to the server).
- No persistent per-item state leaks across queue clears — `headers` is a normal queue-table column, cleared/freed exactly like `artwork_url`/`title` are today when a queue item is removed.
- Match existing code style exactly — this is someone else's C codebase, not greenfield.
- `headers` is accepted as a single already-formatted string (`"Key: Value\r\nKey2: Value2"`), not a JSON object — this matches how `title`/`artist`/`artwork_url` are already passed as plain query-string values (not JSON-encoded) on these same endpoints, and matches the single-string shape ffmpeg's own `headers` AVOption and the new DB column both use. This is a deliberate deviation from the request's suggested `{"Referer": "..."}` JSON shape, per the request's own "defer to whatever fits OwnTone's existing conventions better."
- This cannot be fully verified end-to-end in an automated/headless session — the acceptance criterion "a URL that 403s without the header and succeeds with it" requires a live, real-world YouTube CDN URL (they expire quickly) and network access. See Task 6.

---

### Task 1: Core transcode.c support for per-request headers

**Files:**
- Modify: `src/transcode.c:1375-1447` (`open_input`), `src/transcode.c:1929-1968` (`transcode_decode_setup`)
- Modify: `src/transcode.h:68-78` (`struct transcode_decode_setup_args`)
- Test: `src/transcode_headers_test.c` (new standalone manual test, following the precedent of `src/misc_channels_test.c`)

**Interfaces:**
- Produces: `struct transcode_decode_setup_args.headers` (a `const char *`, may be `NULL`) — consumed by Task 4 (`input_http.c`'s `setup()`, which populates `decode_args.headers`).

- [ ] **Step 1: Add the `headers` field to `struct transcode_decode_setup_args`**

In `src/transcode.h`, inside `struct transcode_decode_setup_args` (lines 68-78), add a field right after `bool is_http;`:

```c
struct transcode_decode_setup_args
{
  enum transcode_profile profile;
  struct media_quality *quality;
  bool is_http;
  // Already-formatted "Key: Value\r\nKey2: Value2" string, or NULL for none.
  // Only meaningful when is_http is true. Not copied/stored anywhere past
  // open_input() -- av_dict_set() below copies it into ffmpeg's own AVDictionary.
  const char *headers;
  uint32_t len_ms;

  // Source must be either of these
  const char *path;
  struct transcode_evbuf_io *evbuf_io;
};
```

- [ ] **Step 2: Extend `open_input()`'s signature to accept the headers string**

In `src/transcode.c`, change `open_input()`'s signature (line 1375-1376) and add the `av_dict_set` call inside the existing `if (ctx->is_http)` block (around line 1390-1397):

```c
static int
open_input(struct decode_ctx *ctx, const char *path, const char *headers, struct transcode_evbuf_io *evbuf_io, enum probe_type probe_type)
{
  AVDictionary *options = NULL;
  AVCodecContext *dec_ctx;
#if USE_CONST_AVFORMAT
  const AVInputFormat *ifmt;
#else
  AVInputFormat *ifmt;
#endif
  unsigned int stream_index;
  const char *user_agent;
  int ret = 0;

  CHECK_NULL(L_XCODE, ctx->ifmt_ctx = avformat_alloc_context());

  if (probe_type == PROBE_TYPE_QUICK)
    {
      ctx->ifmt_ctx->probesize = 65536;
      ctx->ifmt_ctx->format_probesize = 65536;
    }

  if (ctx->is_http)
    {
      av_dict_set(&options, "icy", "1", 0);

      user_agent = cfg_getstr(cfg_getsec(cfg, "general"), "user_agent");
      av_dict_set(&options, "user_agent", user_agent, 0);

      av_dict_set(&options, "reconnect", "1", 0);
      // reconnect_at_eof disabled, breaks m3u8 streams
      av_dict_set(&options, "reconnect_streamed", "1", 0);

      // Per-item custom headers (e.g. Referer), if the queue item set any.
      // headers is NULL for the overwhelming majority of HTTP items, so skip
      // the call entirely rather than setting an empty "headers" option.
      if (headers && *headers)
	av_dict_set(&options, "headers", headers, 0);
    }
```
(The rest of the function — starting from `ctx->ifmt_ctx->interrupt_callback.callback = decode_interrupt_cb;` — is unchanged.)

- [ ] **Step 3: Update `open_input()`'s two call sites in `transcode_decode_setup()`**

In `src/transcode.c`, inside `transcode_decode_setup()` (lines 1945-1954), pass `args.headers` through:

```c
  if (args.is_http)
    {
      ctx->is_http = true;

      ret = open_input(ctx, args.path, args.headers, args.evbuf_io, PROBE_TYPE_QUICK);

      // Retry with a default, slower probe size
      if (ret == AVERROR_STREAM_NOT_FOUND)
	ret = open_input(ctx, args.path, args.headers, args.evbuf_io, PROBE_TYPE_DEFAULT);
    }
  else
    ret = open_input(ctx, args.path, NULL, args.evbuf_io, PROBE_TYPE_DEFAULT);
```

- [ ] **Step 4: Write the standalone test**

This test doesn't call ffmpeg — it can't, without a real HTTP server — so it verifies the one piece of pure logic this task adds: the "skip av_dict_set entirely for NULL/empty headers" guard, isolated as a tiny local helper mirroring the real code's condition (since `open_input`'s real body isn't unit-testable without linking the entire transcode.c + ffmpeg + a live input; this test exists to catch a regression in the specific empty-string-vs-NULL guard logic, not to exercise ffmpeg).

```c
// src/transcode_headers_test.c
// Manual test: gcc -o /tmp/transcode_headers_test src/transcode_headers_test.c
#include <assert.h>
#include <stdio.h>

// Mirrors the exact guard added to open_input() in transcode.c -- kept here
// as a small, isolated regression check since open_input() itself can't be
// unit-tested without linking ffmpeg and a live HTTP source.
static int
should_set_headers(const char *headers)
{
  return headers && *headers;
}

int
main(void)
{
  assert(should_set_headers(NULL) == 0);
  assert(should_set_headers("") == 0);
  assert(should_set_headers("Referer: https://www.youtube.com/") != 0);
  printf("All transcode_headers tests passed.\n");
  return 0;
}
```

- [ ] **Step 5: Compile and run it**

Run: `gcc -o /tmp/transcode_headers_test src/transcode_headers_test.c && /tmp/transcode_headers_test`
Expected: `All transcode_headers tests passed.`

- [ ] **Step 6: Verify the real transcode.c changes build**

This environment may have known, pre-existing gaps preventing a full `make` build (missing ffmpeg/libconfuse/libevent dev headers — confirmed repeatedly in this repo's history). If so, verify by careful reading instead: re-read the final `open_input()`/`transcode_decode_setup()` bodies and confirm every one of `open_input`'s 3 call sites (2 in the `is_http` branch, 1 in the `else` branch) was updated with the new parameter, and that the parameter list order matches between the definition and every call site.

- [ ] **Step 7: Commit**

```bash
git add src/transcode.c src/transcode.h src/transcode_headers_test.c
git commit -m "Add per-request HTTP headers support to transcode.c's open_input"
```

---

### Task 2: DB layer — `headers` column on the queue table

**Files:**
- Modify: `src/db.h:509-563` (`struct db_queue_item`)
- Modify: `src/db_init.h:28-29` (`SCHEMA_VERSION_MINOR`)
- Modify: `src/db_init.c:179-211` (`T_QUEUE`)
- Modify: `src/db_upgrade.c` (new migration block + switch wiring)
- Modify: `src/db.c` (`qi_cols_map[]`, `qi_mfi_map[]`, `free_queue_item()`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `char *headers;` field on `struct db_queue_item` — consumed by Task 3 (`queue_item_stream_add()` sets it) and Task 5 (`jsonapi_reply_queue_tracks_update()` reads/writes it via the existing `update_str()` helper).

**Why this mirrors `artwork_url`, not `title`/`artist`:** `artwork_url` is the one existing queue-item field with NO counterpart in the scanned-library `files` table — it only ever applies to queue items, never to a `media_file_info` from a library scan. `headers` is the same shape: it only makes sense for a manually-queued HTTP stream item, never for a scanned library file. Its `qi_mfi_map` entry therefore uses the same `-1, -1` ("no mfi/dbmfi counterpart, must be set directly on `qi`") sentinel `artwork_url` already uses — no changes to `struct media_file_info`, `mfi_cols_map`, or `dbmfi_cols_map` are needed at all.

- [ ] **Step 1: Add the field to `struct db_queue_item`**

In `src/db.h`, inside `struct db_queue_item` (lines 509-563), add right after `char *artwork_url;`:

```c
  char *artwork_url;
  // Already-formatted "Key: Value\r\nKey2: Value2" HTTP request headers for
  // DATA_KIND_HTTP items, or NULL. Like artwork_url, this has no counterpart
  // in the files table -- it only ever applies to manually-queued streams.
  char *headers;
```

- [ ] **Step 2: Bump the schema version**

In `src/db_init.h:29`, change:
```c
#define SCHEMA_VERSION_MINOR 4
```
to:
```c
#define SCHEMA_VERSION_MINOR 5
```

- [ ] **Step 3: Add the column to the CREATE TABLE (fresh installs)**

In `src/db_init.c`, `T_QUEUE` (lines 179-211), change the last line from:
```c
  "   channels            INTEGER DEFAULT 0"				\
  ");"
```
to:
```c
  "   channels            INTEGER DEFAULT 0,"				\
  "   headers             VARCHAR(4096) DEFAULT NULL"			\
  ");"
```

- [ ] **Step 4: Add the migration for existing DBs**

In `src/db_upgrade.c`, right after the `U_v2204_*` block (after line 1303, before `/* -------------------------- Main upgrade handler -------------------------- */`):

```c
/* ---------------------------- 22.04 -> 22.05 ------------------------------ */

#define U_v2205_ALTER_QUEUE_ADD_HEADERS \
  "ALTER TABLE queue ADD COLUMN headers VARCHAR(4096) DEFAULT NULL;"

#define U_v2205_SCVER_MAJOR                    \
  "UPDATE admin SET value = '22' WHERE key = 'schema_version_major';"
#define U_v2205_SCVER_MINOR                    \
  "UPDATE admin SET value = '05' WHERE key = 'schema_version_minor';"

static const struct db_upgrade_query db_upgrade_v2205_queries[] =
  {
    { U_v2205_ALTER_QUEUE_ADD_HEADERS, "alter table queue add column headers" },

    { U_v2205_SCVER_MAJOR,    "set schema_version_major to 22" },
    { U_v2205_SCVER_MINOR,    "set schema_version_minor to 05" },
  };
```

- [ ] **Step 5: Wire the new migration into the upgrade switch**

In `src/db_upgrade.c`, find the current terminal case (`case 2203:` — read the actual current file first, since Task 2/Plan A's earlier migration already changed this once; the pattern is: the second-to-last case falls through, the last case ends with `break`):

```c
    case 2203:
      ret = db_generic_upgrade(hdl, db_upgrade_v2204_queries, ARRAY_SIZE(db_upgrade_v2204_queries));
      if (ret < 0)
	return -1;

      /* FALLTHROUGH */

    case 2204:
      ret = db_generic_upgrade(hdl, db_upgrade_v2205_queries, ARRAY_SIZE(db_upgrade_v2205_queries));
      if (ret < 0)
	return -1;

      /* Last case statement is the only one that ends with a break statement! */
      break;
```
(Read the actual current file to confirm `case 2203` is indeed the current terminal case before editing — it was, as of this plan being written, but confirm rather than assume.)

- [ ] **Step 6: Add `headers` to `qi_cols_map[]`**

In `src/db.c`, `qi_cols_map[]` (lines 271-303), add right after the `channels` entry (the last one):

```c
    { "channels",           qi_offsetof(channels),            DB_TYPE_INT },
    { "headers",            qi_offsetof(headers),             DB_TYPE_STRING, DB_FIXUP_NO_SANITIZE },
  };
```
(`DB_FIXUP_NO_SANITIZE` matches `path`/`artwork_url`/`virtual_path` — header values must not go through the name-sanitization fixups meant for artist/title text.)

- [ ] **Step 7: Add the matching `qi_mfi_map[]` entry**

In `src/db.c`, `qi_mfi_map[]` (lines 455-486), add right after the `channels` entry (the last one), using the `-1, -1` sentinel exactly like `artwork_url`'s entry a few lines above it:

```c
    { qi_offsetof(channels),            mfi_offsetof(channels),            dbmfi_offsetof(channels) },
    { qi_offsetof(headers),             -1,                                -1 },
```
This array's length must stay in exact 1:1 sync with `qi_cols_map[]` — `src/db.c` has a `static_assert(ARRAY_SIZE(qi_cols_map) == ARRAY_SIZE(qi_mfi_map), ...)` a few thousand lines further down that will fail to compile if this entry is missing or the two arrays drift out of order.

- [ ] **Step 8: Free `headers` in `free_queue_item()`**

In `src/db.c`, `free_queue_item()` (lines 858-878), add right after `free(qi->type);` (the last field-free before `if (!content_only)`):

```c
  free(qi->type);
  free(qi->headers);

  if (!content_only)
    free(qi);
```

- [ ] **Step 9: Verify with the sqlite3 CLI** (this doesn't require the full C toolchain, only the `sqlite3` binary)

```bash
rm -f /tmp/test_queue.db
sqlite3 /tmp/test_queue.db "CREATE TABLE queue (id INTEGER PRIMARY KEY AUTOINCREMENT, file_id INTEGER NOT NULL, pos INTEGER NOT NULL, shuffle_pos INTEGER NOT NULL, data_kind INTEGER NOT NULL, media_kind INTEGER NOT NULL, song_length INTEGER NOT NULL, path VARCHAR(4096) NOT NULL, virtual_path VARCHAR(4096) NOT NULL, title VARCHAR(1024), artist VARCHAR(1024), album_artist VARCHAR(1024) NOT NULL, album VARCHAR(1024) NOT NULL, genre VARCHAR(255), songalbumid INTEGER NOT NULL, time_modified INTEGER DEFAULT 0, artist_sort VARCHAR(1024), album_sort VARCHAR(1024), album_artist_sort VARCHAR(1024), year INTEGER DEFAULT 0, track INTEGER DEFAULT 0, disc INTEGER DEFAULT 0, artwork_url VARCHAR(4096), queue_version INTEGER DEFAULT 0, composer VARCHAR(1024), songartistid INTEGER NOT NULL, type VARCHAR(8), bitrate INTEGER DEFAULT 0, samplerate INTEGER DEFAULT 0, channels INTEGER DEFAULT 0, headers VARCHAR(4096) DEFAULT NULL);"
sqlite3 /tmp/test_queue.db "INSERT INTO queue (file_id,pos,shuffle_pos,data_kind,media_kind,song_length,path,virtual_path,album_artist,album,songalbumid,songartistid) VALUES (1,0,0,5,1,0,'https://example.com/x','x','','',1,1);"
sqlite3 /tmp/test_queue.db "UPDATE queue SET headers = 'Referer: https://www.youtube.com/' WHERE id = 1;"
sqlite3 /tmp/test_queue.db "SELECT id, path, headers FROM queue;"
```
Expected: one row printed with `headers` showing the Referer string.

- [ ] **Step 10: Commit**

```bash
git add src/db.h src/db_init.h src/db_init.c src/db_upgrade.c src/db.c
git commit -m "Add headers column to the queue table"
```

---

### Task 3: Thread `headers` through the queue-add path

**Files:**
- Modify: `src/library.h` (`struct queue_item_add_param`? — no, confirm: it's actually `library_source.queue_item_add` function-pointer typedef and `library_queue_item_add()`'s declaration), `src/library.c` (`struct queue_item_add_param`, `queue_item_add()` dispatcher, `library_queue_item_add()`)
- Modify: `src/library/filescanner.c` (`queue_item_add()`, `queue_item_stream_add()`)
- Modify: `src/library/spotify_webapi.c` (`spotifywebapi_library_queue_item_add()` — signature-only change, no behavior change)

**Interfaces:**
- Consumes: `struct db_queue_item.headers` (Task 2).
- Produces: `library_queue_item_add(const char *path, int position, char reshuffle, uint32_t item_id, const char *headers, int *count, int *new_item_id)` — consumed by Task 5 (`queue_tracks_add_byuris()` in `httpd_jsonapi.c`).

- [ ] **Step 1: Add `headers` to `struct queue_item_add_param`**

In `src/library.c`, `struct queue_item_add_param` (around line 60-68), add a field:

```c
struct queue_item_add_param
{
  const char *path;
  int position;
  char reshuffle;
  uint32_t item_id;
  const char *headers;
  int *count;
  int *new_item_id;
};
```

- [ ] **Step 2: Thread it through the `queue_item_add()` command dispatcher**

In `src/library.c`, `queue_item_add()` (the `static enum command_state` version that dispatches to `sources[i]->queue_item_add`), change the call:

```c
      ret = sources[i]->queue_item_add(param->path, param->position, param->reshuffle, param->item_id, param->headers, param->count, param->new_item_id);
```

- [ ] **Step 3: Update `library_queue_item_add()`'s signature and body**

In `src/library.c`, `library_queue_item_add()`:

```c
int
library_queue_item_add(const char *path, int position, char reshuffle, uint32_t item_id, const char *headers, int *count, int *new_item_id)
{
  struct queue_item_add_param param;
  int count_internal;
  int new_item_id_internal;

  if (library_is_scanning())
    return -1;

  param.path = path;
  param.position = position;
  param.reshuffle = reshuffle;
  param.item_id = item_id;
  param.headers = headers;
  param.count = count ? count : &count_internal;
  param.new_item_id = new_item_id ? new_item_id : &new_item_id_internal;

  return commands_exec_sync(cmdbase, queue_item_add, NULL, &param);
}
```
(Read the actual current function to confirm the final `commands_exec_sync(...)` call/return — shown here from the investigation, but the file may have small differences; keep everything else in the function body identical, only add the `param.headers = headers;` line and the new parameter.)

- [ ] **Step 4: Update `library_queue_item_add()`'s declaration**

In `src/library.h`, find the existing declaration (`library_queue_item_add(const char *path, int position, char reshuffle, uint32_t item_id, int *count, int *new_item_id);`) and the `library_source.queue_item_add` function-pointer field's typedef/signature, and add `const char *headers` in the same position (after `item_id`) in both.

- [ ] **Step 5: Update `filescanner.c`'s `queue_item_add()` and `queue_item_stream_add()`**

In `src/library/filescanner.c`, update both functions' signatures and thread `headers` through to where `qi.headers` gets set (right after `db_queue_item_from_mfi(&qi, &mfi)`, since that call `memset`s the whole struct to 0 first — confirmed by reading `db_queue_item_from_mfi()` in db.c — so `qi.headers` is guaranteed NULL until explicitly set here):

```c
static int
queue_item_stream_add(const char *path, int position, char reshuffle, uint32_t item_id, const char *headers, int *count, int *new_item_id)
{
  struct media_file_info mfi = { 0 };
  struct db_queue_item qi;
  struct db_queue_add_info queue_add_info;
  int ret;

  scan_metadata_stream(&mfi, path);

  db_queue_item_from_mfi(&qi, &mfi);
  qi.headers = safe_strdup(headers); // safe_strdup(NULL) returns NULL, matching the "no headers" default

  ret = db_queue_add_start(&queue_add_info, position);
  if (ret < 0)
    goto error;

  ret = db_queue_add_next(&queue_add_info, &qi);
  ret = db_queue_add_end(&queue_add_info, reshuffle, item_id, ret);
  if (ret < 0)
    goto error;

  if (count)
    *count = queue_add_info.count;
  if (new_item_id)
    *new_item_id = queue_add_info.new_item_id;

  free_queue_item(&qi, 1);
  free_mfi(&mfi, 1);
  return 0;

 error:
  free_queue_item(&qi, 1);
  free_mfi(&mfi, 1);
  return -1;
}

static int
queue_item_add(const char *uri, int position, char reshuffle, uint32_t item_id, const char *headers, int *count, int *new_item_id)
{
  int ret;

  if (strncmp(uri, "library:", strlen("library:")) == 0)
    ret = queue_item_file_add(uri + strlen("library:"), position, reshuffle, item_id, count, new_item_id);
  else if (net_is_http_or_https(uri))
    ret = queue_item_stream_add(uri, position, reshuffle, item_id, headers, count, new_item_id);
  else
    ret = -1;

  return (ret == 0) ? LIBRARY_OK : LIBRARY_PATH_INVALID;
}
```
(`queue_item_file_add()` — the `library:` prefix branch — deliberately does NOT receive `headers`: local library files never need custom HTTP headers, and the request's acceptance criteria don't ask for it there. Confirm `safe_strdup` is already available in filescanner.c's includes — it's declared in `misc.h`, already used throughout this codebase.)

- [ ] **Step 6: Update `spotify_webapi.c`'s signature (no behavior change)**

In `src/library/spotify_webapi.c`, `spotifywebapi_library_queue_item_add()` (line 2188), add the new parameter to the signature only, unused in the body:

```c
static int
spotifywebapi_library_queue_item_add(const char *uri, int position, char reshuffle, uint32_t item_id, const char *headers, int *count, int *new_item_id)
{
```
(Read the rest of the function body first — it's unchanged; only the signature gains the parameter so it still matches the `library_source.queue_item_add` function-pointer type from Step 4. If the compiler warns about an unused parameter, check whether this codebase's build uses `-Wunused-parameter` as an error — if so, mark it `__attribute__((unused))` or reference it in a comment the way other intentionally-unused parameters in this codebase are handled; check for an existing precedent, e.g. `(void)headers;`, before inventing a new convention.)

- [ ] **Step 7: Verify**

Same environment caveat as Task 1/2 — attempt a build if possible (`make -C src library.o library/filescanner.o library/spotify_webapi.o`), otherwise verify by careful reading: confirm every one of the 5 call sites (`queue_item_add()` dispatcher, `library_queue_item_add()`, `filescanner.c`'s `queue_item_add()`+`queue_item_stream_add()`, `spotify_webapi.c`'s implementation) has the new parameter in the exact same position, and that `struct library_source`'s function-pointer field type (in `library.h`) matches all implementers' actual signatures exactly (a mismatch here is a silent, painful bug in C — the compiler won't always catch a function-pointer signature mismatch through an implicit cast, depending on how `sources[]` is declared/populated).

- [ ] **Step 8: Commit**

```bash
git add src/library.c src/library.h src/library/filescanner.c src/library/spotify_webapi.c
git commit -m "Thread per-item HTTP headers through the queue-add path"
```

---

### Task 4: `struct input_source` + `input.c` + `input_http.c` threading

**Files:**
- Modify: `src/input.h:43-78` (`struct input_source`)
- Modify: `src/input.c:369-374` (`clear`), `src/input.c:445-502` (`setup`)
- Modify: `src/inputs/http.c:315-343` (`setup`)

**Interfaces:**
- Consumes: `struct db_queue_item.headers` (Task 2).
- Produces: `struct input_source.headers` (a `char *`, owned/freed like `path`) — consumed by `inputs/http.c`'s `setup()`, which threads it into `struct transcode_decode_setup_args.headers` (Task 1).

- [ ] **Step 1: Add the field to `struct input_source`**

In `src/input.h`, inside `struct input_source` (lines 43-78), add right after `char *path;`:

```c
  char *path;
  // Already-formatted "Key: Value\r\nKey2: Value2" HTTP request headers, or
  // NULL. Only meaningful for data_kind == DATA_KIND_HTTP. Owned/freed the
  // same way path is.
  char *headers;
```

- [ ] **Step 2: Free it in `clear()`**

In `src/input.c`, `clear()` (lines 369-374):

```c
static void
clear(struct input_source *source)
{
  free(source->path);
  free(source->headers);
  memset(source, 0, sizeof(struct input_source));
}
```

- [ ] **Step 3: Populate it in `setup()`**

In `src/input.c`, `setup()` (lines 445-502), add right after `source->path = safe_strdup(queue_item->path);`:

```c
  source->path       = safe_strdup(queue_item->path);
  source->headers    = safe_strdup(queue_item->headers); // safe_strdup(NULL) -> NULL
  source->evbase     = evbase_input;
```

- [ ] **Step 4: Thread it into `transcode_decode_setup_args` in `inputs/http.c`**

In `src/inputs/http.c`, `setup()` (lines 315-343), add `.headers` to the `decode_args` initializer:

```c
static int
setup(struct input_source *source)
{
  struct transcode_decode_setup_args decode_args = { .profile = XCODE_PCM_NATIVE, .is_http = true, .headers = source->headers, .len_ms = source->len_ms };
  struct transcode_encode_setup_args encode_args = { .profile = XCODE_PCM_NATIVE, };
  struct transcode_ctx *ctx;
  char *url;

  if (http_stream_setup(&url, source->path) < 0)
    return -1;

  free(source->path);
  source->path = url;
  decode_args.path = url;

  ctx = transcode_setup(decode_args, encode_args);
  if (!ctx)
    return -1;

  CHECK_NULL(L_PLAYER, source->evbuf = evbuffer_new());

  source->quality.sample_rate = transcode_encode_query(ctx->encode_ctx, "sample_rate");
  source->quality.bits_per_sample = transcode_encode_query(ctx->encode_ctx, "bits_per_sample");
  source->quality.channels = transcode_encode_query(ctx->encode_ctx, "channels");

  source->input_ctx = ctx;

  return 0;
}
```
(Only the `decode_args` initializer line changes — everything else in the function is unchanged, shown in full here only so the whole function's context is unambiguous.)

- [ ] **Step 5: (Lower priority, not required by the acceptance criteria) Thread headers into the libcurl playlist-sniffing path too**

`http_stream_setup()` (`src/http.c:276+`) only performs an actual libcurl HTTP request when the URL's path ends in `.m3u` or `.pls` (playlist sniffing) — for a bare CDN URL with no such extension (the actual YouTube-CDN use case this feature targets), it returns immediately via `strdup(url)` without ever making a request, so this path is NOT exercised by the acceptance criteria. Confirm this by reading the current `http_stream_setup()` body before deciding whether to spend time on it. If you do add it: `struct http_client_ctx` already has an `output_headers` field (a `struct keyval *` linked list consumed by `http_client_request()` at `src/http.c:144-154` via `CURLOPT_HTTPHEADER`) — you would need to (a) change `http_stream_setup()`'s signature to accept `const char *headers`, (b) parse the `\r\n`-joined string into a `struct keyval` list (check `src/misc.h`/`src/http.h` for an existing keyval-list builder helper before writing a new parser), and (c) set it on `ctx.output_headers` before the `http_client_request(&ctx, NULL)` call. Given this isn't required by the acceptance criteria and adds real parsing-code risk, treat this step as optional — do it only if time remains after Task 6 passes, and get explicit sign-off before spending non-trivial time on it.

- [ ] **Step 6: Verify**

Same environment caveat as prior tasks — attempt `make -C src input.o inputs/http.o` if possible, otherwise verify by reading: confirm `source->headers` is freed exactly once (in `clear()`), confirm it's set from `queue_item->headers` (which itself came from a fresh `db_queue_fetch_byitemid()` call in `input.c`'s `start()` command handler, per the existing investigation — re-confirm this by reading `start()` in `input.c`), and confirm the `decode_args` initializer's new `.headers = source->headers` doesn't clash with the existing `.is_http = true` / `.len_ms = source->len_ms` fields (all three coexist in the same designated-initializer list).

- [ ] **Step 7: Commit**

```bash
git add src/input.h src/input.c src/inputs/http.c
git commit -m "Thread per-item HTTP headers from queue item through to input_http"
```

---

### Task 5: JSON API — accept `headers` on add and update

**Files:**
- Modify: `src/httpd_jsonapi.c` (`queue_tracks_add_byuris()`, `jsonapi_reply_queue_tracks_add()`, `jsonapi_reply_queue_tracks_update()`, `queue_item_to_json()`)

**Interfaces:**
- Consumes: `library_queue_item_add()`'s new signature (Task 3), `struct db_queue_item.headers` (Task 2).
- Produces: `"headers"` field in queue-item JSON objects; `headers` query param accepted on `POST /api/queue/items/add` and `PUT /api/queue/items/{id}`.

- [ ] **Step 1: Thread `headers` through `queue_tracks_add_byuris()`**

In `src/httpd_jsonapi.c`, change `queue_tracks_add_byuris()`'s signature and its call to `library_queue_item_add()`:

```c
static int
queue_tracks_add_byuris(const char *param, const char *headers, char shuffle, uint32_t item_id, int pos, int *total_count, int *new_item_id)
{
  char *uris;
  const char *uri;
  char *ptr;
  int count;
  int new;
  int ret;

  *total_count = 0;
  *new_item_id = -1;

  CHECK_NULL(L_WEB, uris = strdup(param));

  uri = strtok_r(uris, ",", &ptr);
  if (!uri)
    {
      DPRINTF(E_LOG, L_WEB, "Empty query parameter 'uris'\n");
      goto error;
    }

  for (; uri; uri = strtok_r(NULL, ",", &ptr))
    {
      ret = library_queue_item_add(uri, pos, shuffle, item_id, headers, &count, &new);
      if (ret != LIBRARY_OK)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid uri '%s'\n", uri);
	  goto error;
	}

      *total_count += count;
      if (pos >= 0)
	pos += count;
      if (*new_item_id == -1)
        *new_item_id = new;
    }

  free(uris);
  return 0;

 error:
  free(uris);
  return -1;
}
```
(One `headers` string applies to every URI in a single, possibly-comma-separated `uris=` call — this is a deliberate scoping decision, not an oversight: the real use case queues one URL at a time, and per-URI-distinct headers within a single batched call is not required by the acceptance criteria.)

- [ ] **Step 2: Read the `headers` query param in `jsonapi_reply_queue_tracks_add()` and pass it through**

In `src/httpd_jsonapi.c`, `jsonapi_reply_queue_tracks_add()`, add right after where `param_uris`/`param_expression` are read (around line 2486-2487):

```c
  param_uris = httpd_query_value_find(hreq->query, "uris");
  param_expression = httpd_query_value_find(hreq->query, "expression");
  param_headers = httpd_query_value_find(hreq->query, "headers");
```
(Add `const char *param_headers;` to the function's local variable declarations at the top, alongside the existing `param_uris`/`param_expression`/`param` declarations.)

Then update the call site (around line 2514-2517):

```c
  if (param_uris)
    {
      ret = queue_tracks_add_byuris(param_uris, param_headers, status.shuffle, status.item_id, pos, &total_count, &new_item_id);
    }
```
(`param_headers` is passed as-is, including when `NULL` — `queue_tracks_add_byuris()` → `library_queue_item_add()` → ... → `safe_strdup(headers)` in `queue_item_stream_add()` already handles a `NULL` headers value correctly, per Task 3 Step 5.)

- [ ] **Step 3: Accept `headers` in `jsonapi_reply_queue_tracks_update()` (`PUT /api/queue/items/{id}`)**

In `src/httpd_jsonapi.c`, `jsonapi_reply_queue_tracks_update()`, add right after the existing `artwork_url` block:

```c
  if ((param = httpd_query_value_find(hreq->query, "artwork_url")))
    update_str(&is_changed, &queue_item->artwork_url, param);
  if ((param = httpd_query_value_find(hreq->query, "headers")))
    update_str(&is_changed, &queue_item->headers, param);
```
(Exactly matches the existing `update_str(&is_changed, &queue_item->X, param)` idiom already used for every other string field in this function — no new pattern introduced.)

- [ ] **Step 4: Expose `headers` in `queue_item_to_json()`**

Find `queue_item_to_json()` in `src/httpd_jsonapi.c` (referenced by `create_reply_queue_tracks_add()` and the main queue-listing endpoint) and add, right after wherever `artwork_url`/`channels` is added to the JSON object (read the actual current function to find the exact line — `channels` was added most recently, per `json_object_object_add(item, "channels", json_object_new_int(queue_item->channels));` seen in this investigation):

```c
  if (queue_item->headers)
    json_object_object_add(item, "headers", json_object_new_string(queue_item->headers));
```
(Guarded on non-NULL, following the pattern of other optional/nullable string fields in this function — check how `artwork_url` is added for the exact precedent to match, since some nullable fields in this function may unconditionally call `json_object_new_string(NULL)`, which json-c handles by producing a JSON `null` rather than crashing, but match whatever the existing convention actually is rather than guessing.)

- [ ] **Step 5: Verify with a live instance, if buildable in this environment**

If a full build is possible:
```bash
sudo make install
sudo /usr/sbin/owntone -f -t &
curl -s "http://localhost:3689/api/queue/items/add?uris=https://example.com/test.mp3&headers=Referer:%20https://example.com/" | python3 -m json.tool
curl -s "http://localhost:3689/api/queue" | python3 -m json.tool | grep -A1 headers
```
Expected: the queued item's JSON includes `"headers": "Referer: https://example.com/"`.

If this environment can't build the full project (confirmed recurring gap in prior work on this repo), verify by careful reading instead: trace the parameter from `httpd_query_value_find(hreq->query, "headers")` all the way to `queue_item_to_json()`'s output, confirming every intermediate function signature matches (this is the same multi-hop chain Task 3/4 already built — this task is the one place all of it gets exercised together).

- [ ] **Step 6: Commit**

```bash
git add src/httpd_jsonapi.c
git commit -m "Expose per-item HTTP headers in the queue JSON API"
```

---

### Task 6: Manual end-to-end verification with a real YouTube CDN URL (blocking — do not skip or claim success without this)

**Files:** none — verification only.

- [ ] **Step 1: Get a real YouTube CDN URL that requires the Referer header**

```bash
yt-dlp -g -f bestaudio "https://www.youtube.com/watch?v=<some-video-id>"
```
This prints a direct `https://...googlevideo.com/...` URL. These expire (typically within hours), so this step must be done immediately before the test, not reused from an earlier session.

- [ ] **Step 2: Confirm the URL actually 403s without the header** (establishes a real negative baseline, not an assumption)

```bash
curl -s -o /dev/null -w "%{http_code}\n" "<the-cdn-url>"
```
Expected: `403`. If it's not 403, this specific URL doesn't demonstrate the bug this feature fixes — get a different video/URL, since not all YouTube CDN URLs require the Referer header (behavior varies).

- [ ] **Step 3: Confirm it succeeds with the header, via plain curl** (isolates "is this a real, fixable HTTP problem" from "did OwnTone's code work")

```bash
curl -s -o /dev/null -w "%{http_code}\n" -H "Referer: https://www.youtube.com/" "<the-cdn-url>"
```
Expected: `200` (or `206`).

- [ ] **Step 4: Queue it in OwnTone with the header set, and confirm it actually plays**

```bash
curl -X POST "http://<host>:3689/api/queue/items/add?uris=<url-encoded-cdn-url>&headers=Referer:%20https://www.youtube.com/&clear=true"
curl -X PUT "http://<host>:3689/api/player/play"
sleep 3
curl -s "http://<host>:3689/api/player"
```
Expected: `"state": "play"`, `item_progress_ms` advancing on a repeated call — i.e., audio is actually decoding and playing, not stuck/failed. Also check `/var/log/owntone.log` for any ffmpeg-level HTTP error during this test.

- [ ] **Step 5: Confirm the no-headers case still works (regression check)**

Queue and play an ordinary internet radio stream URL (no `headers` param) exactly as before this feature existed, and confirm it plays normally — this is the "no regression for plain URLs" acceptance criterion.

- [ ] **Step 6: Report back**

Only after Steps 4 and 5 both pass should this feature be described as working — this cannot be verified in an automated/headless session (no real network access to YouTube's CDN, and CDN URLs expire too quickly to pre-stage).
