/*
 * Copyright (C) 2017 Christian Meffert <christian.meffert@googlemail.com>
 *
 * Adapted from httpd_adm.c:
 * Copyright (C) 2015 Stuart NAIFEH <stu@naifeh.org>
 *
 * Adapted from httpd_daap.c and httpd.c:
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2010 Kai Elwert <elwertk@googlemail.com>
 *
 * Adapted from mt-daapd:
 * Copyright (C) 2003-2007 Ron Pedde <ron@pedde.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "httpd_internal.h"
#include "conffile.h"
#include "db.h"
#include "http.h"
#ifdef LASTFM
# include "lastfm.h"
#endif
#include "library.h"
#include "listenbrainz.h"
#include "logger.h"
#include "misc.h"
#include "misc_json.h"
#include "player.h"
#include "remote_pairing.h"
#include "settings.h"
#include "smartpl_query.h"
#ifdef SPOTIFY
# include "library/spotify_webapi.h"
# include "inputs/spotify.h"
#endif

struct track_attribs
{
  enum library_attrib type;
  const char *name;
};

// Currently these must all be uint32
static const struct track_attribs track_attribs[] =
{
  { LIBRARY_ATTRIB_PLAY_COUNT, "play_count", },
  { LIBRARY_ATTRIB_SKIP_COUNT, "skip_count", },
  { LIBRARY_ATTRIB_TIME_PLAYED, "time_played", },
  { LIBRARY_ATTRIB_TIME_SKIPPED, "time_skipped", },
  { LIBRARY_ATTRIB_RATING, "rating", },
  { LIBRARY_ATTRIB_USERMARK, "usermark", },
};

static bool allow_modifying_stored_playlists;
static char *default_playlist_directory;


/* -------------------------------- HELPERS --------------------------------- */

static bool
is_modified(struct httpd_request *hreq, const char *key)
{
  int64_t db_update = 0;

  db_admin_getint64(&db_update, key);

  return (!db_update || !httpd_request_not_modified_since(hreq, (time_t)db_update));
}

static inline void
safe_json_add_string(json_object *obj, const char *key, const char *value)
{
  if (value)
    json_object_object_add(obj, key, json_object_new_string(value));
}

static inline void
safe_json_add_string_from_int64(json_object *obj, const char *key, int64_t value)
{
  char tmp[100];
  int ret;

  if (value > 0)
    {
      ret = snprintf(tmp, sizeof(tmp), "%" PRIi64, value);
      if (ret < sizeof(tmp))
	json_object_object_add(obj, key, json_object_new_string(tmp));
    }
}

static inline void
safe_json_add_int_from_string(json_object *obj, const char *key, const char *value)
{
  int intval;
  int ret;

  if (!value)
    return;

  ret = safe_atoi32(value, &intval);
  if (ret == 0)
    json_object_object_add(obj, key, json_object_new_int(intval));
}

static inline void
safe_json_add_time_from_string(json_object *obj, const char *key, const char *value)
{
  uint32_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atou32(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to uint32_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (gmtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to gmtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%FT%TZ", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}

static inline void
safe_json_add_date_from_string(json_object *obj, const char *key, const char *value)
{
  int64_t tmp;
  time_t timestamp;
  struct tm tm;
  char result[32];

  if (!value)
    return;

  if (safe_atoi64(value, &tmp) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to int64_t: %s\n", value);
      return;
    }

  if (!tmp)
    return;

  timestamp = tmp;
  if (localtime_r(&timestamp, &tm) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting timestamp to localtime: %s\n", value);
      return;
    }

  strftime(result, sizeof(result), "%F", &tm);

  json_object_object_add(obj, key, json_object_new_string(result));
}

static json_object *
artist_to_json(struct db_group_info *dbgri)
{
  json_object *item;
  int intval;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  safe_json_add_string(item, "id", dbgri->persistentid);
  safe_json_add_string(item, "name", dbgri->itemname);
  safe_json_add_string(item, "name_sort", dbgri->itemname_sort);
  safe_json_add_int_from_string(item, "album_count", dbgri->groupalbumcount);
  safe_json_add_int_from_string(item, "track_count", dbgri->itemcount);
  safe_json_add_int_from_string(item, "length_ms", dbgri->song_length);

  safe_json_add_time_from_string(item, "time_played", dbgri->time_played);
  safe_json_add_time_from_string(item, "time_added", dbgri->time_added);

  ret = safe_atoi32(dbgri->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbgri->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbgri->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "artist", dbgri->persistentid);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/group/%s", dbgri->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  return item;
}

static json_object *
album_to_json(struct db_group_info *dbgri)
{
  json_object *item;
  int intval;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  safe_json_add_string(item, "id", dbgri->persistentid);
  safe_json_add_string(item, "name", dbgri->itemname);
  safe_json_add_string(item, "name_sort", dbgri->itemname_sort);
  safe_json_add_string(item, "artist", dbgri->songalbumartist);
  safe_json_add_string(item, "artist_id", dbgri->songartistid);
  safe_json_add_int_from_string(item, "track_count", dbgri->itemcount);
  safe_json_add_int_from_string(item, "length_ms", dbgri->song_length);

  safe_json_add_time_from_string(item, "time_played", dbgri->time_played);
  safe_json_add_time_from_string(item, "time_added", dbgri->time_added);

  ret = safe_atoi32(dbgri->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbgri->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbgri->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_date_from_string(item, "date_released", dbgri->date_released);
  safe_json_add_int_from_string(item, "year", dbgri->year);

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "album", dbgri->persistentid);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/group/%s", dbgri->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  return item;
}

static json_object *
track_to_json(struct db_media_file_info *dbmfi)
{
  json_object *item;
  char uri[100];
  char artwork_url[100];
  int intval;
  int ret;

  item = json_object_new_object();

  safe_json_add_int_from_string(item, "id", dbmfi->id);
  safe_json_add_string(item, "title", dbmfi->title);
  safe_json_add_string(item, "title_sort", dbmfi->title_sort);
  safe_json_add_string(item, "artist", dbmfi->artist);
  safe_json_add_string(item, "artist_sort", dbmfi->artist_sort);
  safe_json_add_string(item, "album", dbmfi->album);
  safe_json_add_string(item, "album_sort", dbmfi->album_sort);
  safe_json_add_string(item, "album_id", dbmfi->songalbumid);
  safe_json_add_string(item, "album_artist", dbmfi->album_artist);
  safe_json_add_string(item, "album_artist_sort", dbmfi->album_artist_sort);
  safe_json_add_string(item, "album_artist_id", dbmfi->songartistid);
  safe_json_add_string(item, "composer", dbmfi->composer);
  safe_json_add_string(item, "genre", dbmfi->genre);
  safe_json_add_string(item, "comment", dbmfi->comment);
  safe_json_add_int_from_string(item, "year", dbmfi->year);
  safe_json_add_int_from_string(item, "track_number", dbmfi->track);
  safe_json_add_int_from_string(item, "disc_number", dbmfi->disc);
  safe_json_add_int_from_string(item, "length_ms", dbmfi->song_length);

  safe_json_add_int_from_string(item, "rating", dbmfi->rating);
  safe_json_add_int_from_string(item, "play_count", dbmfi->play_count);
  safe_json_add_int_from_string(item, "skip_count", dbmfi->skip_count);
  safe_json_add_time_from_string(item, "time_played", dbmfi->time_played);
  safe_json_add_time_from_string(item, "time_skipped", dbmfi->time_skipped);
  safe_json_add_time_from_string(item, "time_added", dbmfi->time_added);
  safe_json_add_date_from_string(item, "date_released", dbmfi->date_released);
  safe_json_add_int_from_string(item, "seek_ms", dbmfi->seek);

  safe_json_add_string(item, "type", dbmfi->type);
  safe_json_add_int_from_string(item, "samplerate", dbmfi->samplerate);
  safe_json_add_int_from_string(item, "bitrate", dbmfi->bitrate);
  safe_json_add_int_from_string(item, "channels", dbmfi->channels);
  safe_json_add_int_from_string(item, "usermark", dbmfi->usermark);

  ret = safe_atoi32(dbmfi->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbmfi->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_string(item, "path", dbmfi->path);

  ret = snprintf(uri, sizeof(uri), "library:track:%s", dbmfi->id);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  ret = snprintf(artwork_url, sizeof(artwork_url), "/artwork/item/%s", dbmfi->id);
  if (ret < sizeof(artwork_url))
    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));

  safe_json_add_string(item, "lyrics", dbmfi->lyrics);
  return item;
}

static json_object *
playlist_to_json(struct db_playlist_info *dbpli)
{
  json_object *item;
  char uri[100];
  int intval;
  bool boolval;
  int ret;

  item = json_object_new_object();

  safe_json_add_int_from_string(item, "id", dbpli->id);
  safe_json_add_string(item, "name", dbpli->title);
  safe_json_add_string(item, "path", dbpli->path);
  safe_json_add_string(item, "parent_id", dbpli->parent_id);
  ret = safe_atoi32(dbpli->type, &intval);
  if (ret == 0)
    {
      safe_json_add_string(item, "type", db_pl_type_label(intval));
      json_object_object_add(item, "smart_playlist", json_object_new_boolean(intval == PL_SMART));

      boolval = dbpli->query_order && strcasestr(dbpli->query_order, "random");
      json_object_object_add(item, "random", json_object_new_boolean(boolval));

      json_object_object_add(item, "folder", json_object_new_boolean(intval == PL_FOLDER));

      if (intval != PL_FOLDER)
	{
	  safe_json_add_int_from_string(item, "item_count", dbpli->items);
	  safe_json_add_int_from_string(item, "stream_count", dbpli->streams);
	}
    }

  ret = snprintf(uri, sizeof(uri), "%s:%s:%s", "library", "playlist", dbpli->id);
  if (ret < sizeof(uri))
    json_object_object_add(item, "uri", json_object_new_string(uri));

  return item;
}

static json_object *
browse_info_to_json(struct db_browse_info *dbbi)
{
  json_object *item;
  int intval;
  int ret;

  if (dbbi == NULL)
    {
      return NULL;
    }

  item = json_object_new_object();
  safe_json_add_string(item, "name", dbbi->itemname);
  safe_json_add_string(item, "name_sort", dbbi->itemname_sort);
  safe_json_add_int_from_string(item, "track_count", dbbi->track_count);
  safe_json_add_int_from_string(item, "album_count", dbbi->album_count);
  safe_json_add_int_from_string(item, "artist_count", dbbi->artist_count);
  safe_json_add_int_from_string(item, "length_ms", dbbi->song_length);

  safe_json_add_time_from_string(item, "time_played", dbbi->time_played);
  safe_json_add_time_from_string(item, "time_added", dbbi->time_added);

  ret = safe_atoi32(dbbi->seek, &intval);
  if (ret == 0)
    json_object_object_add(item, "in_progress", json_object_new_boolean(intval > 0));

  ret = safe_atoi32(dbbi->media_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "media_kind", db_media_kind_label(intval));

  ret = safe_atoi32(dbbi->data_kind, &intval);
  if (ret == 0)
    safe_json_add_string(item, "data_kind", db_data_kind_label(intval));

  safe_json_add_date_from_string(item, "date_released", dbbi->date_released);
  safe_json_add_int_from_string(item, "year", dbbi->year);

  return item;
}

static json_object *
directory_to_json(struct directory_info *directory_info)
{
  json_object *item;

  if (directory_info == NULL)
    {
      return NULL;
    }

  item = json_object_new_object();
  safe_json_add_string(item, "path", directory_info->path);
//  json_object_object_add(item, "id", json_object_new_int(directory_info->id));
//  json_object_object_add(item, "parent_id", json_object_new_int(directory_info->parent_id));

  return item;
}


static int
fetch_tracks(struct query_params *query_params, json_object *items, int *total)
{
  struct db_media_file_info dbmfi;
  json_object *item;
  int ret;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_file(&dbmfi, query_params)) == 0)
    {
      item = track_to_json(&dbmfi);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static int
fetch_artists(struct query_params *query_params, json_object *items, int *total)
{
  struct db_group_info dbgri;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_group(&dbgri, query_params)) == 0)
    {
      /* Don't add item if no name (eg blank album name) */
      if (strlen(dbgri.itemname) == 0)
	continue;

      item = artist_to_json(&dbgri);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_artist(bool *notfound, const char *artist_id)
{
  struct query_params query_params;
  json_object *artist;
  struct db_group_info dbgri;
  int ret = 0;

  *notfound = true;
  memset(&query_params, 0, sizeof(struct query_params));
  artist = NULL;

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;
  query_params.filter = db_mprintf("(f.songartistid = %s)", artist_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_group(&dbgri, &query_params)) == 0)
    {
      artist = artist_to_json(&dbgri);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return artist;
}

static int
fetch_albums(struct query_params *query_params, json_object *items, int *total)
{
  struct db_group_info dbgri;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_group(&dbgri, query_params)) == 0)
    {
      /* Don't add item if no name (eg blank album name) */
      if (strlen(dbgri.itemname) == 0)
	continue;

      item = album_to_json(&dbgri);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_album(bool *notfound, const char *album_id)
{
  struct query_params query_params;
  json_object *album;
  struct db_group_info dbgri;
  int ret = 0;

  *notfound = true;

  memset(&query_params, 0, sizeof(struct query_params));
  album = NULL;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songalbumid = %s)", album_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_group(&dbgri, &query_params)) == 0)
    {
      album = album_to_json(&dbgri);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return album;
}

static int
fetch_playlists(struct query_params *query_params, json_object *items, int *total)
{
  struct db_playlist_info dbpli;
  json_object *item;
  int ret = 0;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while (((ret = db_query_fetch_pl(&dbpli, query_params)) == 0) && (dbpli.id))
    {
      item = playlist_to_json(&dbpli);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static json_object *
fetch_playlist(bool *notfound, uint32_t playlist_id)
{
  struct query_params query_params;
  json_object *playlist;
  struct db_playlist_info dbpli;
  int ret = 0;

  *notfound = true;

  memset(&query_params, 0, sizeof(struct query_params));
  playlist = NULL;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("(f.id = %d)", playlist_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if (((ret = db_query_fetch_pl(&dbpli, &query_params)) == 0) && (dbpli.id))
    {
      playlist = playlist_to_json(&dbpli);
      *notfound = false;
    }

 error:
  db_query_end(&query_params);
  free(query_params.filter);

  return playlist;
}

static int
fetch_browse_info(struct query_params *query_params, json_object *items, int *total)
{
  struct db_browse_info dbbi;
  json_object *item;
  int ret;

  ret = db_query_start(query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_query_fetch_browse(&dbbi, query_params)) == 0)
    {
      item = browse_info_to_json(&dbbi);
      if (!item)
	{
	  ret = -1;
	  goto error;
	}

      json_object_array_add(items, item);
    }

  if (total)
    *total = query_params->results;

 error:
  db_query_end(query_params);

  return ret;
}

static int
fetch_directories(int parent_id, json_object *items)
{
  json_object *item;
  int ret;
  struct directory_info subdir;
  struct directory_enum dir_enum;

  memset(&dir_enum, 0, sizeof(struct directory_enum));
  dir_enum.parent_id = parent_id;
  ret = db_directory_enum_start(&dir_enum);
  if (ret < 0)
    goto error;

  while ((ret = db_directory_enum_fetch(&dir_enum, &subdir)) == 0 && subdir.id > 0)
    {
      item = directory_to_json(&subdir);
      if (!item)
      {
	ret = -1;
	goto error;
      }

      json_object_array_add(items, item);
    }

 error:
  db_directory_enum_end(&dir_enum);

  return ret;
}


static int
query_params_limit_set(struct query_params *query_params, struct httpd_request *hreq)
{
  const char *param;

  query_params->idx_type = I_NONE;
  query_params->limit = -1;
  query_params->offset = 0;

  param = httpd_query_value_find(hreq->query, "limit");
  if (param)
    {
      query_params->idx_type = I_SUB;

      if (safe_atoi32(param, &query_params->limit) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid value for query parameter 'limit' (%s)\n", param);
	  return -1;
	}

      param = httpd_query_value_find(hreq->query, "offset");
      if (param && safe_atoi32(param, &query_params->offset) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid value for query parameter 'offset' (%s)\n", param);
	  return -1;
	}
    }

  return 0;
}

/* --------------------------- REPLY HANDLERS ------------------------------- */

/*
 * Endpoint to retrieve configuration values
 *
 * Example response:
 *
 * {
 *  "websocket_port": 6603,
 *  "version": "25.0"
 * }
 */
static int
jsonapi_reply_config(struct httpd_request *hreq)
{
  json_object *jreply;
  json_object *buildopts;
  int websocket_port;
  char **buildoptions;
  cfg_t *lib;
  int ndirs;
  char *path;
  char *deref;
  json_object *directories;
  int i;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  // library name
  json_object_object_add(jreply, "library_name", json_object_new_string(cfg_getstr(cfg_getsec(cfg, "library"), "name")));

  // hide singles
  json_object_object_add(jreply, "hide_singles", json_object_new_boolean(cfg_getbool(cfg_getsec(cfg, "library"), "hide_singles")));

  // Websocket port
#ifdef HAVE_LIBWEBSOCKETS
  websocket_port = cfg_getint(cfg_getsec(cfg, "general"), "websocket_port");
#else
  websocket_port = 0;
#endif
  json_object_object_add(jreply, "websocket_port", json_object_new_int(websocket_port));

  // server version
  json_object_object_add(jreply, "version", json_object_new_string(VERSION));

  // enabled build options
  buildopts = json_object_new_array();
  buildoptions = buildopts_get();
  for (i = 0; buildoptions[i]; i++)
    {
      json_object_array_add(buildopts, json_object_new_string(buildoptions[i]));
    }
  json_object_object_add(jreply, "buildoptions", buildopts);

  // Library directories
  lib = cfg_getsec(cfg, "library");
  ndirs = cfg_size(lib, "directories");
  directories = json_object_new_array();
  for (i = 0; i < ndirs; i++)
    {
      path = cfg_getnstr(lib, "directories", i);

      // The path in the conf file may have a trailing slash character. Return the realpath like it is done in the bulk_scan function in filescanner.c
      deref = realpath(path, NULL);
      if (deref)
        {
	  json_object_array_add(directories, json_object_new_string(deref));
	  free(deref);
	}
      else
	{
	  DPRINTF(E_LOG, L_WEB, "Skipping library directory %s, could not dereference: %s\n", path, strerror(errno));
	}
    }
  json_object_object_add(jreply, "directories", directories);
  json_object_object_add(jreply, "radio_playlists", json_object_new_boolean(cfg_getbool(lib, "radio_playlists")));

  // Config for creating/modifying stored playlists
  json_object_object_add(jreply, "allow_modifying_stored_playlists", json_object_new_boolean(allow_modifying_stored_playlists));
  safe_json_add_string(jreply, "default_playlist_directory", default_playlist_directory);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static json_object *
option_get_json(struct settings_option *option)
{
  const char *optionname;
  json_object *json_option;
  int intval;
  bool boolval;
  char *strval;


  optionname = option->name;

  CHECK_NULL(L_WEB, json_option = json_object_new_object());
  json_object_object_add(json_option, "name", json_object_new_string(option->name));
  json_object_object_add(json_option, "type", json_object_new_int(option->type));

  if (option->type == SETTINGS_TYPE_INT)
    {
      intval = settings_option_getint(option);
      json_object_object_add(json_option, "value", json_object_new_int(intval));
    }
  else if (option->type == SETTINGS_TYPE_BOOL)
    {
      boolval = settings_option_getbool(option);
      json_object_object_add(json_option, "value", json_object_new_boolean(boolval));
    }
  else if (option->type == SETTINGS_TYPE_STR)
    {
      strval = settings_option_getstr(option);
      if (strval)
	{
	  json_object_object_add(json_option, "value", json_object_new_string(strval));
	  free(strval);
	}
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Option '%s' has unknown type %d\n", optionname, option->type);
      jparse_free(json_option);
      return NULL;
    }

  return json_option;
}

static json_object *
category_get_json(struct settings_category *category)
{
  json_object *json_category;
  json_object *json_options;
  json_object *json_option;
  struct settings_option *option;
  int count;
  int i;

  json_category = json_object_new_object();

  json_object_object_add(json_category, "name", json_object_new_string(category->name));

  json_options = json_object_new_array();

  count = settings_option_count(category);
  for (i = 0; i < count; i++)
    {
      option = settings_option_get_byindex(category, i);
      json_option = option_get_json(option);
      if (json_option)
	json_object_array_add(json_options, json_option);
    }

  json_object_object_add(json_category, "options", json_options);

  return json_category;
}

static int
jsonapi_reply_settings_get(struct httpd_request *hreq)
{
  struct settings_category *category;
  json_object *jreply;
  json_object *json_categories;
  json_object *json_category;
  int count;
  int i;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_categories = json_object_new_array();

  count = settings_categories_count();
  for (i = 0; i < count; i++)
    {
      category = settings_category_get_byindex(i);
      json_category = category_get_json(category);
      if (json_category)
	json_object_array_add(json_categories, json_category);
    }

  json_object_object_add(jreply, "categories", json_categories);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_category_get(struct httpd_request *hreq)
{
  const char *categoryname;
  struct settings_category *category;
  json_object *jreply;


  categoryname = hreq->path_parts[2];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  jreply = category_get_json(category);

  if (!jreply)
    {
      DPRINTF(E_LOG, L_WEB, "Error getting value for category '%s'\n", categoryname);
      return HTTP_INTERNAL;
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_option_get(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object *jreply;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);
  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  jreply = option_get_json(option);

  if (!jreply)
    {
      DPRINTF(E_LOG, L_WEB, "Error getting value for option '%s'\n", optionname);
      return HTTP_INTERNAL;
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_settings_option_put(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  json_object* request;
  int intval;
  bool boolval;
  const char *strval;
  int ret;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);

  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Missing request body for setting option '%s' (type %d)\n", optionname, option->type);
      return HTTP_BADREQUEST;
    }

  if (option->type == SETTINGS_TYPE_INT && jparse_contains_key(request, "value", json_type_int))
    {
      intval = jparse_int_from_obj(request, "value");
      ret = settings_option_setint(option, intval);
    }
  else if (option->type == SETTINGS_TYPE_BOOL && jparse_contains_key(request, "value", json_type_boolean))
    {
      boolval = jparse_bool_from_obj(request, "value");

      if (boolval && strcasecmp(categoryname, "webinterface") == 0 && strcasecmp(optionname, "require_auth_lan") == 0)
	{
	  struct settings_option *password_option = settings_option_get(category, "auth_password");
	  const char *db_password = password_option ? settings_option_getstr(password_option) : NULL;
	  const char *cfg_password = cfg_getstr(cfg_getsec(cfg, "general"), "admin_password");

	  if ((!db_password || !*db_password) && (!cfg_password || !*cfg_password))
	    {
	      DPRINTF(E_LOG, L_WEB, "Refusing to enable 'require_auth_lan' with no password set\n");
	      return HTTP_BADREQUEST;
	    }
	}

      ret = settings_option_setbool(option, boolval);
    }
  else if (option->type == SETTINGS_TYPE_STR && jparse_contains_key(request, "value", json_type_string))
    {
      strval = jparse_str_from_obj(request, "value");
      ret = settings_option_setstr(option, strval);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid value given for option '%s' (type %d): '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_BADREQUEST;
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error changing setting '%s' (type %d) to '%s'\n", optionname, option->type, json_object_to_json_string(request));
      return HTTP_INTERNAL;
    }

  DPRINTF(E_INFO, L_WEB, "Setting option '%s.%s' changed to '%s'\n", categoryname, optionname, json_object_to_json_string(request));
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_settings_option_delete(struct httpd_request *hreq)
{
  const char *categoryname;
  const char *optionname;
  struct settings_category *category;
  struct settings_option *option;
  int ret;


  categoryname = hreq->path_parts[2];
  optionname = hreq->path_parts[3];

  category = settings_category_get(categoryname);
  if (!category)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid category name '%s' given\n", categoryname);
      return HTTP_NOTFOUND;
    }

  option = settings_option_get(category, optionname);
  if (!option)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid option name '%s' given\n", optionname);
      return HTTP_NOTFOUND;
    }

  ret = settings_option_delete(option);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error deleting option '%s'\n", optionname);
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

/*
 * Endpoint to retrieve informations about the library
 *
 * Example response:
 *
 * {
 *  "artists": 84,
 *  "albums": 151,
 *  "songs": 3085,
 *  "db_playtime": 687824,
 *  "updating": false
 *}
 */
static int
jsonapi_reply_library(struct httpd_request *hreq)
{
  struct query_params qp;
  struct filecount_info fci;
  json_object *jreply;
  int ret;
  char *s;
  int i;
  struct library_source **sources;
  json_object *jscanners;
  json_object *jsource;


  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_COUNT_ITEMS;
  ret = db_filecount_get(&fci, &qp);
  if (ret == 0)
    {
      json_object_object_add(jreply, "songs", json_object_new_int(fci.count));
      json_object_object_add(jreply, "db_playtime", json_object_new_int64((fci.length / 1000)));
      json_object_object_add(jreply, "artists", json_object_new_int(fci.artist_count));
      json_object_object_add(jreply, "albums", json_object_new_int(fci.album_count));
      json_object_object_add(jreply, "file_size", json_object_new_int64(fci.file_size));
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "library: failed to get file count info\n");
    }

  ret = db_admin_get(&s, DB_ADMIN_START_TIME);
  if (ret == 0)
    {
      safe_json_add_time_from_string(jreply, "started_at", s);
      free(s);
    }

  ret = db_admin_get(&s, DB_ADMIN_DB_UPDATE);
  if (ret == 0)
    {
      safe_json_add_time_from_string(jreply, "updated_at", s);
      free(s);
    }

  json_object_object_add(jreply, "updating", json_object_new_boolean(library_is_scanning()));

  jscanners = json_object_new_array();
  json_object_object_add(jreply, "scanners", jscanners);
  sources = library_sources();
  for (i = 0; sources[i]; i++)
    {
      if (!sources[i]->disabled)
	{
	  jsource = json_object_new_object();
	  safe_json_add_string(jsource, "name", db_scan_kind_label(sources[i]->scan_kind));
	  json_object_array_add(jscanners, jsource);
	}
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * Endpoint to trigger a library rescan
 */
static int
jsonapi_reply_update(struct httpd_request *hreq)
{
  const char *param;

  param = httpd_query_value_find(hreq->query, "scan_kind");

  library_rescan(db_scan_kind_enum(param));
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_meta_rescan(struct httpd_request *hreq)
{
  const char *param;

  param = httpd_query_value_find(hreq->query, "scan_kind");

  library_metarescan(db_scan_kind_enum(param));
  return HTTP_NOCONTENT;
}


/*
 * Endpoint to retrieve information about the spotify integration
 *
 * Exampe response:
 *
 * {
 *  "enabled": true,
 *  "oauth_uri": "https://accounts.spotify.com/authorize/?client_id=...
 * }
 */
/* Hard wall-clock limit for the "timeout"-wrapped yt-dlp invocation below */
#define YOUTUBE_RESOLVE_TIMEOUT_SECS 20
#define YOUTUBE_RESOLVE_TIMEOUT_STR "20"

/* Runs argv[0] (searched via PATH, no shell involved) with argv as its
 * arguments, captures its stdout into a malloc'd, NUL-terminated buffer.
 * Caller is expected to have wrapped the command with the "timeout" binary
 * as argv[0] if a hard time limit is needed (see resolve_youtube_stream_url()).
 * Returns NULL if the child could not be started, stdout could not be read,
 * or the child exited with a non-zero status/was killed by a signal.
 */
static char *
run_argv_capture_output(char *const argv[], int timeout_secs)
{
  int outpipe[2];
  pid_t pid;
  char *output = NULL;
  size_t total_len = 0;
  char buffer[4096];
  ssize_t n;
  int status;

  if (pipe(outpipe) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not create pipe for '%s': %s\n", argv[0], strerror(errno));
      return NULL;
    }

  pid = fork();
  if (pid < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not fork to run '%s': %s\n", argv[0], strerror(errno));
      close(outpipe[0]);
      close(outpipe[1]);
      return NULL;
    }

  if (pid == 0)
    {
      /* Child.
       *
       * If owntone dropped privileges via runas (main.c uses seteuid()/
       * setegid(), which only change the effective ids -- real/saved stay
       * root), this process has a real uid/gid that differs from its
       * effective uid/gid. The kernel marks any subsequent execve() from
       * such a process as a "secure exec" (AT_SECURE=1), which makes the
       * dynamic linker silently ignore LD_LIBRARY_PATH. yt-dlp's bundled
       * Python interpreter relies on exactly that env var (set by its own
       * PyInstaller bootloader) to find its bundled libcrypto/_ssl instead
       * of the system's -- without it, it silently falls back to the
       * system OpenSSL and fails to load if that's an incompatible version.
       * Confirmed live: this was the actual cause of yt-dlp reliably
       * failing only when spawned from owntone, never when run standalone.
       * Normalize real/effective/saved ids to match before exec -- shedding
       * the leftover root real-id costs nothing here (this child only ever
       * runs yt-dlp) and removes the mismatch that triggers AT_SECURE.
       * setresuid()/setresgid() to the CURRENT effective id are always
       * permitted even without any privileged capability (unlike plain
       * setuid(), which needs CAP_SETUID -- already lost the moment the
       * effective id left root), so these cannot fail here in a way that
       * should block launching yt-dlp; log and continue on error rather
       * than aborting the whole resolve over it.
       */
      uid_t euid = geteuid();
      gid_t egid = getegid();

      if (getuid() != euid && setresuid(euid, euid, euid) < 0)
        DPRINTF(E_LOG, L_WEB, "Could not normalize uid before running '%s': %s\n", argv[0], strerror(errno));
      if (getgid() != egid && setresgid(egid, egid, egid) < 0)
        DPRINTF(E_LOG, L_WEB, "Could not normalize gid before running '%s': %s\n", argv[0], strerror(errno));

      /* owntone is a long-running daemon with a large, ever-changing set of
       * open file descriptors (RAOP/AirPlay sockets, mDNS, the sqlite db
       * handle, libevent eventfds/signalfds, pipes, ...) -- none of them
       * are meant for a one-shot yt-dlp invocation; close everything but
       * the three standard streams before exec.
       */
      int devnull = open("/dev/null", O_RDONLY);
      if (devnull >= 0)
        {
          dup2(devnull, STDIN_FILENO);
          if (devnull > STDIN_FILENO)
            close(devnull);
        }

      if (dup2(outpipe[1], STDOUT_FILENO) < 0)
        _exit(127);

      {
        long openmax = sysconf(_SC_OPEN_MAX);
        int fd;
        int maxfd = (openmax > 0 && openmax < 65536) ? (int)openmax : 1024;

        for (fd = 3; fd < maxfd; fd++)
          close(fd);
      }

      execvp(argv[0], argv);
      _exit(127); /* execvp only returns on failure */
    }

  /* Parent */
  close(outpipe[1]);

  while ((n = read(outpipe[0], buffer, sizeof(buffer))) > 0)
    {
      char *tmp = realloc(output, total_len + n + 1);
      if (!tmp)
        {
          DPRINTF(E_LOG, L_WEB, "Out of memory reading output of '%s'\n", argv[0]);
          free(output);
          output = NULL;
          break;
        }

      output = tmp;
      memcpy(output + total_len, buffer, n);
      total_len += n;
      output[total_len] = '\0';
    }

  close(outpipe[0]);

  if (waitpid(pid, &status, 0) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "waitpid() failed for '%s': %s\n", argv[0], strerror(errno));
      free(output);
      return NULL;
    }

  if (n < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error reading output of '%s': %s\n", argv[0], strerror(errno));
      free(output);
      return NULL;
    }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
      DPRINTF(E_LOG, L_WEB, "'%s' exited with an error (timeout %ds)\n", argv[0], timeout_secs);
      free(output);
      return NULL;
    }

  return output;
}

/* Returns true only for recognised YouTube video URL shapes:
 *   http(s)://[www.]youtube.com/watch?v=...
 *   http(s)://youtu.be/...
 *   http(s)://[www.]youtube.com/shorts/...
 * Host match is case-insensitive; this is a simple prefix check, not a full
 * URL parser.
 */
static bool
is_youtube_url(const char *url)
{
  const char *p = url;

  if (strncasecmp(p, "http://", 7) == 0)
    p += 7;
  else if (strncasecmp(p, "https://", 8) == 0)
    p += 8;
  else
    return false;

  if (strncasecmp(p, "www.", 4) == 0)
    p += 4;

  if (strncasecmp(p, "youtube.com/watch?v=", 20) == 0)
    return (strlen(p + 20) > 0);

  if (strncasecmp(p, "youtube.com/shorts/", 19) == 0)
    return (strlen(p + 19) > 0);

  if (strncasecmp(p, "youtu.be/", 9) == 0)
    return (strlen(p + 9) > 0);

  return false;
}

/* Resolves a validated YouTube URL to a title and a playable stream URL by
 * shelling out to yt-dlp (via run_argv_capture_output(), no shell parsing).
 * On success returns 0 and sets *title and *stream_url to malloc'd strings
 * that the caller must free. On failure returns -1 and leaves *title and
 * *stream_url untouched.
 *
 * Exposed (non-static-only in intent) so a future search/queue-all endpoint
 * can call it directly instead of duplicating the yt-dlp invocation.
 */
static int
resolve_youtube_stream_url(const char *url, char **title, char **stream_url)
{
  char *argv[] = {
    "timeout", YOUTUBE_RESOLVE_TIMEOUT_STR,
    "yt-dlp", "--no-warnings", "--skip-download",
    /* m4a/mp4 over the generic bestaudio selector (usually opus/webm):
     * ffmpeg can't seek an opus/webm stream fetched over plain HTTP, and
     * some webm renditions fail to open via OwnTone's http input at all,
     * while m4a's moov-atom index makes it reliably seekable/playable. */
    "-f", "bestaudio[ext=m4a]/bestaudio",
    "--print", "%(title)s",
    "--print", "%(urls)s",
    (char *)url,
    NULL
  };
  char *output;
  char *sep;

  output = run_argv_capture_output(argv, YOUTUBE_RESOLVE_TIMEOUT_SECS);
  if (!output)
    return -1;

  sep = strchr(output, '\n');
  if (!sep)
    {
      DPRINTF(E_LOG, L_WEB, "Unexpected yt-dlp output for '%s' (missing title/url separator)\n", url);
      free(output);
      return -1;
    }

  *sep = '\0';

  *title = output;
  *stream_url = strdup(sep + 1);
  if (!*stream_url)
    {
      free(*title);
      *title = NULL;
      return -1;
    }

  /* Trim a single trailing newline (and stray CR) yt-dlp appends after the URL */
  sep = strpbrk(*stream_url, "\r\n");
  if (sep)
    *sep = '\0';

  if (strlen(*title) == 0 || strlen(*stream_url) == 0)
    {
      DPRINTF(E_LOG, L_WEB, "yt-dlp returned an empty title or stream URL for '%s'\n", url);
      free(*title);
      free(*stream_url);
      *title = NULL;
      *stream_url = NULL;
      return -1;
    }

  return 0;
}

/* Parses a YouTube Data API ISO 8601 duration ("PT4M13S", "PT1H2M", "PT30S")
 * into whole seconds. Returns 0 for anything unparseable rather than failing
 * the whole search reply over a missing/malformed duration on one item. */
static int
parse_iso8601_duration(const char *iso)
{
  const char *p = iso;
  int hours = 0, minutes = 0, seconds = 0;
  int value;

  if (!p || p[0] != 'P' || p[1] != 'T')
    return 0;

  p += 2;
  while (*p)
    {
      value = (int)strtol(p, (char **)&p, 10);
      if (!*p)
	break;

      switch (*p)
	{
	  case 'H': hours = value; break;
	  case 'M': minutes = value; break;
	  case 'S': seconds = value; break;
	  default: return 0;
	}
      p++;
    }

  return hours * 3600 + minutes * 60 + seconds;
}

/* Fetches durations (in seconds) for a batch of YouTube video ids via the
 * Data API's videos.list endpoint (part=contentDetails), adding a
 * "duration" field (int seconds) to each object in jresults whose
 * "video_id" matches. Best-effort: any failure here just leaves results
 * without a duration rather than failing the whole search reply, since the
 * search itself already succeeded. */
static void
add_youtube_durations(json_object *jresults, const char *api_key, const char *video_ids_csv)
{
  struct keyval *kv = NULL;
  char *querystring = NULL;
  char *url = NULL;
  struct http_client_ctx ctx = { 0 };
  json_object *response = NULL;
  json_object *items = NULL;
  const char *body;
  int ret;
  int i;
  int count;

  if (!video_ids_csv || strlen(video_ids_csv) == 0)
    return;

  CHECK_NULL(L_WEB, kv = keyval_alloc());
  if (keyval_add(kv, "part", "contentDetails") < 0
      || keyval_add(kv, "id", video_ids_csv) < 0
      || keyval_add(kv, "key", api_key) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to build YouTube videos.list query parameters\n");
      goto out;
    }

  querystring = http_form_urlencode(kv);
  if (!querystring)
    goto out;

  url = safe_asprintf("https://www.googleapis.com/youtube/v3/videos?%s", querystring);

  ctx.url = url;
  ctx.input_body = evbuffer_new();
  if (!ctx.input_body)
    goto out;

  ret = http_client_request(&ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Did not get a reply from YouTube Data API (videos.list)\n");
      goto out;
    }

  evbuffer_add(ctx.input_body, "", 1);
  body = (char *)evbuffer_pullup(ctx.input_body, -1);
  if (!body || strlen(body) == 0 || ctx.response_code < 200 || ctx.response_code >= 300)
    {
      DPRINTF(E_LOG, L_WEB, "Bad reply from YouTube Data API (videos.list), status %d\n", ctx.response_code);
      goto out;
    }

  response = json_tokener_parse(body);
  if (!response || !json_object_object_get_ex(response, "items", &items) || json_object_get_type(items) != json_type_array)
    goto out;

  count = json_object_array_length(items);
  for (i = 0; i < count; i++)
    {
      json_object *item = json_object_array_get_idx(items, i);
      json_object *jcontent_details = NULL;
      const char *id;
      const char *iso_duration;
      int duration_seconds;
      int j;
      int nresults;

      id = jparse_str_from_obj(item, "id");
      if (!id || !json_object_object_get_ex(item, "contentDetails", &jcontent_details))
	continue;

      iso_duration = jparse_str_from_obj(jcontent_details, "duration");
      if (!iso_duration)
	continue;

      duration_seconds = parse_iso8601_duration(iso_duration);

      nresults = json_object_array_length(jresults);
      for (j = 0; j < nresults; j++)
	{
	  json_object *jresult = json_object_array_get_idx(jresults, j);
	  const char *result_video_id = jparse_str_from_obj(jresult, "video_id");

	  if (result_video_id && strcmp(result_video_id, id) == 0)
	    {
	      json_object_object_add(jresult, "duration", json_object_new_int(duration_seconds));
	      break;
	    }
	}
    }

 out:
  if (response)
    jparse_free(response);
  if (ctx.input_body)
    evbuffer_free(ctx.input_body);
  free(url);
  free(querystring);
  if (kv)
    {
      keyval_clear(kv);
      free(kv);
    }
}

static int
jsonapi_reply_spotify(struct httpd_request *hreq)
{
  json_object *jreply;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

#ifdef SPOTIFY
  char *oauth_uri;
  struct spotify_status sp_status;
  struct spotifywebapi_status_info webapi_info;
  struct spotifywebapi_access_token webapi_token;

  json_object_object_add(jreply, "enabled", json_object_new_boolean(true));

  oauth_uri = spotifywebapi_oauth_uri_get();
  if (!oauth_uri)
    {
      DPRINTF(E_LOG, L_WEB, "Cannot display Spotify oauth interface (http_form_uriencode() failed)\n");
      jparse_free(jreply);
      return HTTP_INTERNAL;
    }

  json_object_object_add(jreply, "oauth_uri", json_object_new_string(oauth_uri));
  free(oauth_uri);

  spotify_status_get(&sp_status);
  json_object_object_add(jreply, "spotify_installed", json_object_new_boolean(sp_status.installed));
  json_object_object_add(jreply, "spotify_logged_in", json_object_new_boolean(sp_status.logged_in));
  json_object_object_add(jreply, "has_podcast_support", json_object_new_boolean(sp_status.has_podcast_support));

  spotifywebapi_status_info_get(&webapi_info);
  json_object_object_add(jreply, "webapi_token_valid", json_object_new_boolean(webapi_info.token_valid));
  safe_json_add_string(jreply, "webapi_user", webapi_info.user);
  safe_json_add_string(jreply, "webapi_country", webapi_info.country);
  safe_json_add_string(jreply, "webapi_granted_scope", webapi_info.granted_scope);
  safe_json_add_string(jreply, "webapi_required_scope", webapi_info.required_scope);
  safe_json_add_string(jreply, "webapi_client_id", webapi_info.client_id);
  safe_json_add_string(jreply, "webapi_client_secret", webapi_info.client_secret);

  spotifywebapi_access_token_get(&webapi_token);
  safe_json_add_string(jreply, "webapi_token", webapi_token.token);
  json_object_object_add(jreply, "webapi_token_expires_in", json_object_new_int(webapi_token.expires_in));
  free(webapi_token.token);
#else
  json_object_object_add(jreply, "enabled", json_object_new_boolean(false));
#endif

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_spotify_logout(struct httpd_request *hreq)
{
#ifdef SPOTIFY
  spotifywebapi_purge();
  spotify_logout();
#endif
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_youtube(struct httpd_request *hreq)
{
  json_object *jreply;
  struct settings_category *services_category;
  struct settings_option *api_key_option;
  char *api_key;
  bool configured = false;

  services_category = settings_category_get("services");
  api_key_option = services_category ? settings_option_get(services_category, "youtube_api_key") : NULL;
  api_key = api_key_option ? settings_option_getstr(api_key_option) : NULL;

  if (api_key && strlen(api_key) > 0)
    configured = true;

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_object_object_add(jreply, "enabled", json_object_new_boolean(true));
  json_object_object_add(jreply, "configured", json_object_new_boolean(configured));
  /* The web UI calls the YouTube Data API directly from the browser for
   * search (server-side search added ~2 blocking HTTP round trips on top
   * of an already resource-constrained host) -- it needs the actual key
   * value for that, not just whether one is configured. This UI is only
   * ever served to whoever can already reach this JSON API. */
  if (configured)
    safe_json_add_string(jreply, "api_key", api_key);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  free(api_key);

  return HTTP_OK;
}

static int
jsonapi_reply_youtube_resolve(struct httpd_request *hreq)
{
  json_object *request;
  json_object *jreply;
  const char *url;
  char *title = NULL;
  char *stream_url = NULL;
  bool success;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  url = jparse_str_from_obj(request, "url");
  if (!url || strlen(url) == 0)
    {
      DPRINTF(E_LOG, L_WEB, "No URL in YouTube resolve post request\n");
      jparse_free(request);
      return HTTP_BADREQUEST;
    }

  if (!is_youtube_url(url))
    {
      DPRINTF(E_LOG, L_WEB, "Rejecting YouTube resolve request for non-YouTube URL '%s'\n", url);

      CHECK_NULL(L_WEB, jreply = json_object_new_object());
      json_object_object_add(jreply, "success", json_object_new_boolean(false));
      safe_json_add_string(jreply, "error", "Not a valid YouTube URL");
      CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

      jparse_free(jreply);
      jparse_free(request);
      return HTTP_BADREQUEST;
    }

  success = (resolve_youtube_stream_url(url, &title, &stream_url) == 0);

  CHECK_NULL(L_WEB, jreply = json_object_new_object());
  json_object_object_add(jreply, "success", json_object_new_boolean(success));
  if (title)
    safe_json_add_string(jreply, "title", title);
  if (stream_url)
    safe_json_add_string(jreply, "stream_url", stream_url);
  if (!success)
    safe_json_add_string(jreply, "error", "Failed to resolve YouTube URL");

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  free(title);
  free(stream_url);
  jparse_free(request);
  jparse_free(jreply);

  return HTTP_OK;
}

/* Auto-generated "Mix"/Radio lists (list=RD...) aren't real playlists and
 * can't be read through the Data API's playlistItems endpoint -- the web
 * UI resolves those via the Data API directly client-side for a real
 * playlist, and falls back to this yt-dlp-backed endpoint only for
 * Mix/Radio lists (or any other URL the Data API can't expand). Longer
 * timeout than a single resolve since --flat-playlist still walks the
 * whole (capped) list server-side. */
#define YOUTUBE_PLAYLIST_MAX_ITEMS 50
#define YOUTUBE_PLAYLIST_TIMEOUT_SECS 30
#define YOUTUBE_PLAYLIST_TIMEOUT_STR "30"
#define YOUTUBE_PLAYLIST_MAX_ITEMS_STR "50"

static int
jsonapi_reply_youtube_resolve_playlist(struct httpd_request *hreq)
{
  json_object *request = NULL;
  json_object *jreply = NULL;
  json_object *jresults = NULL;
  json_object *data = NULL;
  json_object *entries = NULL;
  const char *url;
  char *output = NULL;
  int i;
  int count;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  url = jparse_str_from_obj(request, "url");
  if (!url || strlen(url) == 0 || !is_youtube_url(url))
    {
      DPRINTF(E_LOG, L_WEB, "No valid YouTube URL in playlist resolve request\n");
      jparse_free(request);
      return HTTP_BADREQUEST;
    }

  {
    char *argv[] = {
      "timeout", YOUTUBE_PLAYLIST_TIMEOUT_STR,
      "yt-dlp", "--no-warnings", "--flat-playlist",
      "--playlist-end", YOUTUBE_PLAYLIST_MAX_ITEMS_STR,
      "-J", (char *)url,
      NULL
    };

    output = run_argv_capture_output(argv, YOUTUBE_PLAYLIST_TIMEOUT_SECS);
  }

  if (!output)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to resolve YouTube playlist '%s'\n", url);
      jparse_free(request);

      CHECK_NULL(L_WEB, jreply = json_object_new_object());
      safe_json_add_string(jreply, "error", "Could not resolve this playlist");
      CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
      jparse_free(jreply);

      return HTTP_INTERNAL;
    }

  data = json_tokener_parse(output);
  free(output);

  CHECK_NULL(L_WEB, jreply = json_object_new_object());
  CHECK_NULL(L_WEB, jresults = json_object_new_array());

  if (data && json_object_object_get_ex(data, "entries", &entries) && json_object_get_type(entries) == json_type_array)
    {
      count = json_object_array_length(entries);
      for (i = 0; i < count && i < YOUTUBE_PLAYLIST_MAX_ITEMS; i++)
	{
	  json_object *entry = json_object_array_get_idx(entries, i);
	  json_object *jthumbnails = NULL;
	  const char *video_id;
	  const char *title;
	  const char *channel;
	  const char *thumbnail = NULL;
	  double duration = 0;
	  json_object *jresult;
	  char *video_url;

	  video_id = jparse_str_from_obj(entry, "id");
	  if (!video_id)
	    continue;

	  title = jparse_str_from_obj(entry, "title");
	  channel = jparse_str_from_obj(entry, "channel");
	  if (!channel)
	    channel = jparse_str_from_obj(entry, "uploader");
	  duration = jparse_double_from_obj(entry, "duration");

	  if (json_object_object_get_ex(entry, "thumbnails", &jthumbnails) && json_object_get_type(jthumbnails) == json_type_array
	      && json_object_array_length(jthumbnails) > 0)
	    thumbnail = jparse_str_from_obj(json_object_array_get_idx(jthumbnails, json_object_array_length(jthumbnails) - 1), "url");

	  video_url = safe_asprintf("https://www.youtube.com/watch?v=%s", video_id);

	  CHECK_NULL(L_WEB, jresult = json_object_new_object());
	  safe_json_add_string(jresult, "video_id", video_id);
	  if (title)
	    safe_json_add_string(jresult, "title", title);
	  if (channel)
	    safe_json_add_string(jresult, "channel", channel);
	  if (thumbnail)
	    safe_json_add_string(jresult, "thumbnail", thumbnail);
	  safe_json_add_string(jresult, "url", video_url);
	  json_object_object_add(jresult, "duration", json_object_new_int((int)duration));

	  free(video_url);

	  json_object_array_add(jresults, jresult);
	}
    }

  json_object_object_add(jreply, "results", jresults);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  if (data)
    jparse_free(data);
  jparse_free(jreply);
  jparse_free(request);

  return HTTP_OK;
}

#undef YOUTUBE_PLAYLIST_MAX_ITEMS
#undef YOUTUBE_PLAYLIST_TIMEOUT_SECS
#undef YOUTUBE_PLAYLIST_TIMEOUT_STR
#undef YOUTUBE_PLAYLIST_MAX_ITEMS_STR

#define YOUTUBE_SEARCH_DEFAULT_LIMIT 15
#define YOUTUBE_SEARCH_MIN_LIMIT 1
#define YOUTUBE_SEARCH_MAX_LIMIT 25

static int
jsonapi_reply_youtube_search(struct httpd_request *hreq)
{
  json_object *request = NULL;
  json_object *jreply = NULL;
  json_object *jresults = NULL;
  json_object *response = NULL;
  struct settings_category *services_category;
  struct settings_option *api_key_option;
  char *api_key = NULL;
  struct keyval *kv = NULL;
  char *querystring = NULL;
  char *url = NULL;
  struct http_client_ctx ctx = { 0 };
  const char *query;
  const char *body;
  int limit;
  int ret;
  int err;
  const char *errmsg = NULL;
  char limit_buf[16];

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  query = jparse_str_from_obj(request, "query");
  if (!query || strlen(query) == 0)
    {
      DPRINTF(E_LOG, L_WEB, "No query in YouTube search post request\n");
      jparse_free(request);
      return HTTP_BADREQUEST;
    }

  limit = jparse_int_from_obj(request, "limit");
  if (limit <= 0)
    limit = YOUTUBE_SEARCH_DEFAULT_LIMIT;
  if (limit < YOUTUBE_SEARCH_MIN_LIMIT)
    limit = YOUTUBE_SEARCH_MIN_LIMIT;
  else if (limit > YOUTUBE_SEARCH_MAX_LIMIT)
    limit = YOUTUBE_SEARCH_MAX_LIMIT;

  services_category = settings_category_get("services");
  api_key_option = services_category ? settings_option_get(services_category, "youtube_api_key") : NULL;
  api_key = api_key_option ? settings_option_getstr(api_key_option) : NULL;

  if (!api_key || strlen(api_key) == 0)
    {
      DPRINTF(E_LOG, L_WEB, "YouTube search request, but no API key is configured\n");

      CHECK_NULL(L_WEB, jreply = json_object_new_object());
      safe_json_add_string(jreply, "error", "YouTube API key is not configured");
      CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

      jparse_free(jreply);
      jparse_free(request);
      free(api_key);
      return HTTP_BADREQUEST;
    }

  snprintf(limit_buf, sizeof(limit_buf), "%d", limit);

  CHECK_NULL(L_WEB, kv = keyval_alloc());
  if (keyval_add(kv, "part", "snippet") < 0
      || keyval_add(kv, "type", "video") < 0
      || keyval_add(kv, "maxResults", limit_buf) < 0
      || keyval_add(kv, "q", query) < 0
      || keyval_add(kv, "key", api_key) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to build YouTube search query parameters\n");
      err = HTTP_INTERNAL;
      errmsg = "Failed to build request";
      goto error;
    }

  querystring = http_form_urlencode(kv);
  if (!querystring)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to urlencode YouTube search query parameters\n");
      err = HTTP_INTERNAL;
      errmsg = "Failed to build request";
      goto error;
    }

  url = safe_asprintf("https://www.googleapis.com/youtube/v3/search?%s", querystring);

  ctx.url = url;
  ctx.input_body = evbuffer_new();
  if (!ctx.input_body)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to allocate input body for YouTube search request\n");
      err = HTTP_INTERNAL;
      errmsg = "Failed to build request";
      goto error;
    }

  ret = http_client_request(&ctx, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Did not get a reply from YouTube Data API\n");
      err = HTTP_INTERNAL;
      errmsg = "YouTube API request failed";
      goto error;
    }

  // 0-terminate for safety
  evbuffer_add(ctx.input_body, "", 1);

  body = (char *)evbuffer_pullup(ctx.input_body, -1);
  if (!body || strlen(body) == 0)
    {
      DPRINTF(E_LOG, L_WEB, "Empty reply from YouTube Data API\n");
      err = HTTP_INTERNAL;
      errmsg = "Invalid response from YouTube API";
      goto error;
    }

  response = json_tokener_parse(body);
  if (!response)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse YouTube Data API reply as JSON: %s\n", body);
      err = HTTP_INTERNAL;
      errmsg = "Invalid response from YouTube API";
      goto error;
    }

  if (ctx.response_code < 200 || ctx.response_code >= 300)
    {
      json_object *jerror = NULL;

      if (json_object_object_get_ex(response, "error", &jerror))
	errmsg = jparse_str_from_obj(jerror, "message");

      DPRINTF(E_LOG, L_WEB, "YouTube Data API returned error status %d: %s\n", ctx.response_code, body);

      if (!errmsg)
	errmsg = "YouTube Data API request failed";
      err = HTTP_INTERNAL;
      goto error;
    }

  CHECK_NULL(L_WEB, jreply = json_object_new_object());
  CHECK_NULL(L_WEB, jresults = json_object_new_array());

  {
    json_object *items = NULL;
    int i;
    int count;
    char video_ids[YOUTUBE_SEARCH_MAX_LIMIT * 12 + 1] = "";

    if (json_object_object_get_ex(response, "items", &items) && json_object_get_type(items) == json_type_array)
      {
	count = json_object_array_length(items);
	for (i = 0; i < count; i++)
	  {
	    json_object *item = json_object_array_get_idx(items, i);
	    json_object *jid = NULL;
	    json_object *jsnippet = NULL;
	    json_object *jthumbnails = NULL;
	    json_object *jdefault_thumb = NULL;
	    const char *video_id = NULL;
	    const char *title = NULL;
	    const char *channel = NULL;
	    const char *thumbnail = NULL;
	    json_object *jresult;
	    char *video_url;

	    if (json_object_object_get_ex(item, "id", &jid))
	      video_id = jparse_str_from_obj(jid, "videoId");

	    if (!video_id)
	      continue;

	    if (json_object_object_get_ex(item, "snippet", &jsnippet))
	      {
		title = jparse_str_from_obj(jsnippet, "title");
		channel = jparse_str_from_obj(jsnippet, "channelTitle");

		if (json_object_object_get_ex(jsnippet, "thumbnails", &jthumbnails)
		    && json_object_object_get_ex(jthumbnails, "default", &jdefault_thumb))
		  thumbnail = jparse_str_from_obj(jdefault_thumb, "url");
	      }

	    video_url = safe_asprintf("https://www.youtube.com/watch?v=%s", video_id);

	    CHECK_NULL(L_WEB, jresult = json_object_new_object());
	    safe_json_add_string(jresult, "video_id", video_id);
	    if (title)
	      safe_json_add_string(jresult, "title", title);
	    if (channel)
	      safe_json_add_string(jresult, "channel", channel);
	    if (thumbnail)
	      safe_json_add_string(jresult, "thumbnail", thumbnail);
	    safe_json_add_string(jresult, "url", video_url);

	    free(video_url);

	    json_object_array_add(jresults, jresult);

	    if (strlen(video_ids) + strlen(video_id) + 2 < sizeof(video_ids))
	      {
		if (video_ids[0])
		  strcat(video_ids, ",");
		strcat(video_ids, video_id);
	      }
	  }
      }

    add_youtube_durations(jresults, api_key, video_ids);
  }

  json_object_object_add(jreply, "results", jresults);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  jparse_free(response);
  jparse_free(request);
  evbuffer_free(ctx.input_body);
  free(url);
  free(querystring);
  keyval_clear(kv);
  free(kv);
  free(api_key);

  return HTTP_OK;

 error:
  CHECK_NULL(L_WEB, jreply = json_object_new_object());
  safe_json_add_string(jreply, "error", errmsg ? errmsg : "Internal error");
  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  jparse_free(response);
  jparse_free(request);
  if (ctx.input_body)
    evbuffer_free(ctx.input_body);
  free(url);
  free(querystring);
  if (kv)
    {
      keyval_clear(kv);
      free(kv);
    }
  free(api_key);

  return err;
}

#undef YOUTUBE_SEARCH_DEFAULT_LIMIT
#undef YOUTUBE_SEARCH_MIN_LIMIT
#undef YOUTUBE_SEARCH_MAX_LIMIT

static int
jsonapi_reply_lastfm(struct httpd_request *hreq)
{
  json_object *jreply;
  bool enabled = false;
  bool scrobbling_enabled = false;

#ifdef LASTFM
  enabled = true;
  scrobbling_enabled = lastfm_is_enabled();
#endif

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_object_object_add(jreply, "enabled", json_object_new_boolean(enabled));
  json_object_object_add(jreply, "scrobbling_enabled", json_object_new_boolean(scrobbling_enabled));

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * Endpoint to log into LastFM
 */
static int
jsonapi_reply_lastfm_login(struct httpd_request *hreq)
{
#ifdef LASTFM
  json_object *request;
  const char *user;
  const char *password;
  char *errmsg = NULL;
  json_object *jreply;
  json_object *errors;
  int ret;

  DPRINTF(E_DBG, L_WEB, "Received LastFM login request\n");

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  user = jparse_str_from_obj(request, "user");
  password = jparse_str_from_obj(request, "password");
  if (user && strlen(user) > 0 && password && strlen(password) > 0)
    {
      ret = lastfm_login_user(user, password, &errmsg);
      if (ret < 0)
        {
	  json_object_object_add(jreply, "success", json_object_new_boolean(false));
	  errors = json_object_new_object();
	  if (errmsg)
	    json_object_object_add(errors, "error", json_object_new_string(errmsg));
	  else
	    json_object_object_add(errors, "error", json_object_new_string("Unknown error"));
	  json_object_object_add(jreply, "errors", errors);
	}
      else
        {
	  json_object_object_add(jreply, "success", json_object_new_boolean(true));
	}
      free(errmsg);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "No user or password in LastFM login post request\n");

      json_object_object_add(jreply, "success", json_object_new_boolean(false));
      errors = json_object_new_object();
      if (!user || strlen(user) == 0)
	json_object_object_add(errors, "user", json_object_new_string("Username is required"));
      if (!password || strlen(password) == 0)
	json_object_object_add(errors, "password", json_object_new_string("Password is required"));
      json_object_object_add(jreply, "errors", errors);
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

#else
  DPRINTF(E_LOG, L_WEB, "Received LastFM login request but was not compiled with enable-lastfm\n");
#endif

  return HTTP_OK;
}

static int
jsonapi_reply_lastfm_logout(struct httpd_request *hreq)
{
#ifdef LASTFM
  lastfm_logout();
#endif
  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_listenbrainz(struct httpd_request *hreq)
{
  struct listenbrainz_status status;
  json_object *jreply;

  listenbrainz_status_get(&status);

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  json_object_object_add(jreply, "enabled", json_object_new_boolean(!status.disabled));
  json_object_object_add(jreply, "token_valid", json_object_new_boolean(status.token_valid));
  if (status.user_name)
    json_object_object_add(jreply, "user_name", json_object_new_string(status.user_name));
  if (status.message)
    json_object_object_add(jreply, "message", json_object_new_string(status.message));


  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  listenbrainz_status_free(&status, true);

  return HTTP_OK;
}

static int
jsonapi_reply_listenbrainz_token_add(struct httpd_request *hreq)
{
  json_object *request;
  const char *token;
  int ret;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  token = jparse_str_from_obj(request, "token");

  ret = listenbrainz_token_set(token);

  jparse_free(request);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to set ListenBrainz token\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_listenbrainz_token_delete(struct httpd_request *hreq)
{
  int ret;
  
  ret = listenbrainz_token_delete();

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to delete ListenBrainz token\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

/*
 * Kicks off pairing of a daap/dacp client
 *
 * Expects the paring pin to be present in the post request body, e. g.:
 *
 * {
 *   "pin": "1234"
 * }
 */
static int
jsonapi_reply_pairing_pair(struct httpd_request *hreq)
{
  json_object* request;
  const char* pin;
  int ret;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received pairing post request: %s\n", json_object_to_json_string(request));

  pin = jparse_str_from_obj(request, "pin");
  if (pin)
    {
      ret = remote_pairing_pair(pin);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Missing pin in request body: %s\n", json_object_to_json_string(request));
      ret = REMOTE_INVALID_PIN;
    }

  jparse_free(request);

  if (ret == 0)
    return HTTP_NOCONTENT;
  else if (ret == REMOTE_INVALID_PIN)
    return HTTP_BADREQUEST;

  return HTTP_INTERNAL;
}

/*
 * Retrieves pairing information
 *
 * Example response:
 *
 * {
 *  "active": true,
 *  "remote": "remote name"
 * }
 */
static int
jsonapi_reply_pairing_get(struct httpd_request *hreq)
{
  char *remote_name;
  json_object *jreply;

  remote_name = remote_pairing_get_name();

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  if (remote_name)
    {
      json_object_object_add(jreply, "active", json_object_new_boolean(true));
      json_object_object_add(jreply, "remote", json_object_new_string(remote_name));
    }
  else
    {
      json_object_object_add(jreply, "active", json_object_new_boolean(false));
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);
  free(remote_name);

  return HTTP_OK;
}

struct outputs_param
{
  json_object *output;
  uint64_t output_id;
  int output_volume;
};

static json_object *
speaker_to_json(struct player_speaker_info *spk)
{
  json_object *output;
  json_object *supported_formats;
  char output_id[21];
  enum media_format format;

  output = json_object_new_object();

  supported_formats = json_object_new_array();
  for (format = MEDIA_FORMAT_FIRST; format <= MEDIA_FORMAT_LAST; format = MEDIA_FORMAT_NEXT(format))
    {
      if (format & spk->supported_formats)
	json_object_array_add(supported_formats, json_object_new_string(media_format_to_string(format)));
    }

  snprintf(output_id, sizeof(output_id), "%" PRIu64, spk->id);
  json_object_object_add(output, "id", json_object_new_string(output_id));
  json_object_object_add(output, "name", json_object_new_string(spk->name));
  json_object_object_add(output, "type", json_object_new_string(spk->output_type));
  json_object_object_add(output, "selected", json_object_new_boolean(spk->selected));
  json_object_object_add(output, "has_password", json_object_new_boolean(spk->has_password));
  json_object_object_add(output, "requires_auth", json_object_new_boolean(spk->requires_auth));
  json_object_object_add(output, "needs_auth_key", json_object_new_boolean(spk->needs_auth_key));
  json_object_object_add(output, "volume", json_object_new_int(spk->absvol));
  json_object_object_add(output, "offset_ms", json_object_new_int(spk->offset_ms));
  json_object_object_add(output, "channels", json_object_new_string(output_channels_to_string(spk->channels)));
  json_object_object_add(output, "format", json_object_new_string(media_format_to_string(spk->format)));
  json_object_object_add(output, "supported_formats", supported_formats);

  return output;
}

static void
speaker_enum_cb(struct player_speaker_info *spk, void *arg)
{
  json_object *outputs;
  json_object *output;

  outputs = arg;

  output = speaker_to_json(spk);
  json_object_array_add(outputs, output);
}

/*
 * GET /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_get_byid(struct httpd_request *hreq)
{
  struct player_speaker_info speaker_info;
  uint64_t output_id;
  json_object *jreply;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = player_speaker_get_byid(&speaker_info, output_id);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No output found for '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  jreply = speaker_to_json(&speaker_info);
  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

/*
 * PUT /api/outputs/[output_id]
 */
static int
jsonapi_reply_outputs_put_byid(struct httpd_request *hreq)
{
  uint64_t output_id;
  json_object *request = NULL;
  bool selected;
  int volume;
  int offset_ms;
  const char *pin;
  const char *format;
  const char *channels;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);
      goto error;
    }

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      goto error;
    }

  if (jparse_contains_key(request, "selected", json_type_boolean))
    {
      selected = jparse_bool_from_obj(request, "selected");
      ret = selected ? player_speaker_enable(output_id) : player_speaker_disable(output_id);
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "volume", json_type_int))
    {
      volume = jparse_int_from_obj(request, "volume");
      ret = player_volume_setabs_speaker(output_id, volume);
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "pin", json_type_string))
    {
      pin = jparse_str_from_obj(request, "pin");
      ret = pin ? player_speaker_authorize(output_id, pin) : 0;
      if (ret < 0)
	goto error;

    }

  if (jparse_contains_key(request, "format", json_type_string))
    {
      format = jparse_str_from_obj(request, "format");
      ret = format ? player_speaker_format_set(output_id, media_format_from_string(format)) : 0;
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "offset_ms", json_type_int))
    {
      offset_ms = jparse_int_from_obj(request, "offset_ms");
      ret = player_speaker_offset_ms_set(output_id, offset_ms);
      if (ret < 0)
	goto error;
    }

  if (jparse_contains_key(request, "channels", json_type_string))
    {
      channels = jparse_str_from_obj(request, "channels");
      ret = channels ? player_speaker_channels_set(output_id, output_channels_from_string(channels)) : 0;
      if (ret < 0)
	goto error;
    }

  jparse_free(request);
  return HTTP_NOCONTENT;

 error:
  jparse_free(request);
  return HTTP_BADREQUEST;
}

/*
 * PUT /api/outputs/[output_id]/toggle
 */
static int
jsonapi_reply_outputs_toggle_byid(struct httpd_request *hreq)
{
  uint64_t output_id;
  struct player_speaker_info spk;
  int ret;

  ret = safe_atou64(hreq->path_parts[2], &output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid output id given to outputs endpoint '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = player_speaker_get_byid(&spk, output_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No output found for the given output id, toggle failed for '%s'\n", hreq->path);
      return HTTP_BADREQUEST;
    }

  if (spk.selected)
    ret = player_speaker_disable(output_id);
  else
    ret = player_speaker_enable(output_id);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_NOCONTENT;
}

/*
 * Endpoint "/api/outputs"
 */
static int
jsonapi_reply_outputs(struct httpd_request *hreq)
{
  json_object *outputs;
  json_object *jreply;

  outputs = json_object_new_array();

  player_speaker_enumerate(speaker_enum_cb, outputs);

  jreply = json_object_new_object();
  json_object_object_add(jreply, "outputs", outputs);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_verification(struct httpd_request *hreq)
{
  json_object* request;
  const char* message;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received verification post request: %s\n", json_object_to_json_string(request));

  message = jparse_str_from_obj(request, "pin");
  if (message)
    player_raop_verification_kickoff((char **)&message);
  else
    DPRINTF(E_LOG, L_WEB, "Missing pin in request body: %s\n", json_object_to_json_string(request));

  jparse_free(request);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_outputs_set(struct httpd_request *hreq)
{
  json_object *request;
  json_object *outputs;
  json_object *output_id;
  int nspk, i, ret;
  uint64_t *ids;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  DPRINTF(E_DBG, L_WEB, "Received select-outputs post request: %s\n", json_object_to_json_string(request));

  ret = jparse_array_from_obj(request, "outputs", &outputs);
  if (ret == 0)
    {
      nspk = json_object_array_length(outputs);

      CHECK_NULL(L_WEB, ids = calloc((nspk + 1), sizeof(uint64_t)));
      ids[0] = nspk;

      ret = 0;
      for (i = 0; i < nspk; i++)
	{
	  output_id = json_object_array_get_idx(outputs, i);
	  ret = safe_atou64(json_object_get_string(output_id), &ids[i + 1]);
	  if (ret < 0)
	    {
	      DPRINTF(E_LOG, L_WEB, "Failed to convert output id: %s\n", json_object_to_json_string(request));
	      break;
	    }
	}

      if (ret == 0)
	player_speaker_set(ids);

      free(ids);
    }
  else
    DPRINTF(E_LOG, L_WEB, "Missing outputs in request body: %s\n", json_object_to_json_string(request));

  jparse_free(request);

  return HTTP_NOCONTENT;
}

static int
play_item_with_id(const char *param)
{
  uint32_t item_id;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &item_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  queue_item = db_queue_fetch_byitemid(item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item with item id '%d'\n", item_id);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from item with id '%d'\n", item_id);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
play_item_at_position(const char *param)
{
  uint32_t position;
  struct player_status status;
  struct db_queue_item *queue_item;
  int ret;

  ret = safe_atou32(param, &position);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid position given '%s'\n", param);

      return HTTP_BADREQUEST;
    }

  player_get_status(&status);

  queue_item = db_queue_fetch_bypos(position, status.shuffle);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No queue item at position '%d'\n", position);

      return HTTP_BADREQUEST;
    }

  player_playback_stop();
  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to start playback from position '%d'\n", position);

      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_play(struct httpd_request *hreq)
{
  const char *param;
  int ret;

  if ((param = httpd_query_value_find(hreq->query, "item_id")))
    {
      return play_item_with_id(param);
    }
  else if ((param = httpd_query_value_find(hreq->query, "position")))
    {
      return play_item_at_position(param);
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_pause(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_pause();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error pausing playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_stop(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_stop();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error stopping playback.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_toggle(struct httpd_request *hreq)
{
  struct player_status status;
  int ret;

  player_get_status(&status);
  DPRINTF(E_DBG, L_WEB, "Toggle playback request with current state %d.\n", status.status);

  if (status.status == PLAY_PLAYING)
    {
      ret = player_playback_pause();
    }
  else
    {
      ret = player_playback_start();
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error toggling playback state.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_next(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_next();
  if (ret < 0)
    {
      // If skipping to the next song failed, it is most likely we reached the end of the queue,
      // ignore the error (play status change will be reported to the client over the websocket)
      DPRINTF(E_DBG, L_WEB, "Error switching to next item (possibly end of queue reached).\n");
      return HTTP_NOCONTENT;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after switching to next item.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_previous(struct httpd_request *hreq)
{
  int ret;

  ret = player_playback_prev();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error switching to previous item.\n");
      return HTTP_INTERNAL;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after switching to previous item.\n");
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_seek(struct httpd_request *hreq)
{
  const char *param_pos;
  const char *param_seek;
  int position_ms;
  int seek_ms;
  int ret;

  param_pos = httpd_query_value_find(hreq->query, "position_ms");
  param_seek = httpd_query_value_find(hreq->query, "seek_ms");
  if (!param_pos && !param_seek)
    return HTTP_BADREQUEST;

  if (param_pos)
    {
      ret = safe_atoi32(param_pos, &position_ms);
      if (ret < 0)
	return HTTP_BADREQUEST;

      ret = player_playback_seek(position_ms, PLAYER_SEEK_POSITION);
    }
  else
    {
      ret = safe_atoi32(param_seek, &seek_ms);
      if (ret < 0)
	return HTTP_BADREQUEST;

      ret = player_playback_seek(seek_ms, PLAYER_SEEK_RELATIVE);
    }

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error seeking (position_ms=%s, seek_ms=%s).\n",
	      (param_pos ? param_pos : ""), (param_seek ? param_seek : ""));
      return HTTP_INTERNAL;
    }

  ret = player_playback_start();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error starting playback after seeking (position_ms=%s, seek_ms=%s).\n",
	      (param_pos ? param_pos : ""), (param_seek ? param_seek : ""));
      return HTTP_INTERNAL;
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player(struct httpd_request *hreq)
{
  struct player_status status;
  struct db_queue_item *queue_item;
  json_object *reply;

  player_get_status(&status);

  reply = json_object_new_object();

  switch (status.status)
    {
      case PLAY_PAUSED:
	json_object_object_add(reply, "state", json_object_new_string("pause"));
	break;

      case PLAY_PLAYING:
	json_object_object_add(reply, "state", json_object_new_string("play"));
	break;

      default:
	json_object_object_add(reply, "state", json_object_new_string("stop"));
	break;
    }

  switch (status.repeat)
    {
      case REPEAT_SONG:
	json_object_object_add(reply, "repeat", json_object_new_string("single"));
	break;

      case REPEAT_ALL:
	json_object_object_add(reply, "repeat", json_object_new_string("all"));
	break;

      default:
	json_object_object_add(reply, "repeat", json_object_new_string("off"));
	break;
    }

  json_object_object_add(reply, "consume", json_object_new_boolean(status.consume));
  json_object_object_add(reply, "shuffle", json_object_new_boolean(status.shuffle));
  json_object_object_add(reply, "volume", json_object_new_int(status.volume));

  if (status.item_id)
    {
      json_object_object_add(reply, "item_id", json_object_new_int(status.item_id));
      json_object_object_add(reply, "item_length_ms", json_object_new_int(status.len_ms));
      json_object_object_add(reply, "item_progress_ms", json_object_new_int(status.pos_ms));
      json_object_object_add(reply, "artwork_url", json_object_new_string("./artwork/nowplaying"));
    }
  else
    {
      queue_item = db_queue_fetch_bypos(0, status.shuffle);

      if (queue_item)
	{
	  json_object_object_add(reply, "item_id", json_object_new_int(queue_item->id));
	  json_object_object_add(reply, "item_length_ms", json_object_new_int(queue_item->song_length));
	  json_object_object_add(reply, "item_progress_ms", json_object_new_int(0));
	  free_queue_item(queue_item, 0);
	}
      else
	{
	  json_object_object_add(reply, "item_id", json_object_new_int(0));
	  json_object_object_add(reply, "item_length_ms", json_object_new_int(0));
	  json_object_object_add(reply, "item_progress_ms", json_object_new_int(0));
	}
    }

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply)));

  jparse_free(reply);

  return HTTP_OK;
}

static json_object *
queue_item_to_json(struct db_queue_item *queue_item, char shuffle)
{
  json_object *item;
  char uri[100];
  char artwork_url[100];
  int ret;

  item = json_object_new_object();

  json_object_object_add(item, "id", json_object_new_int(queue_item->id));
  if (shuffle)
    json_object_object_add(item, "position", json_object_new_int(queue_item->shuffle_pos));
  else
    json_object_object_add(item, "position", json_object_new_int(queue_item->pos));

  if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    json_object_object_add(item, "track_id", json_object_new_int(queue_item->file_id));

  safe_json_add_string(item, "title", queue_item->title);
  safe_json_add_string(item, "artist", queue_item->artist);
  safe_json_add_string(item, "artist_sort", queue_item->artist_sort);
  safe_json_add_string(item, "album", queue_item->album);
  safe_json_add_string(item, "album_sort", queue_item->album_sort);
  safe_json_add_string_from_int64(item, "album_id", queue_item->songalbumid);
  safe_json_add_string(item, "album_artist", queue_item->album_artist);
  safe_json_add_string(item, "album_artist_sort", queue_item->album_artist_sort);
  safe_json_add_string_from_int64(item, "album_artist_id", queue_item->songartistid);
  safe_json_add_string(item, "composer", queue_item->composer);
  safe_json_add_string(item, "genre", queue_item->genre);

  json_object_object_add(item, "year", json_object_new_int(queue_item->year));
  json_object_object_add(item, "track_number", json_object_new_int(queue_item->track));
  json_object_object_add(item, "disc_number", json_object_new_int(queue_item->disc));
  json_object_object_add(item, "length_ms", json_object_new_int(queue_item->song_length));

  safe_json_add_string(item, "media_kind", db_media_kind_label(queue_item->media_kind));
  safe_json_add_string(item, "data_kind", db_data_kind_label(queue_item->data_kind));

  safe_json_add_string(item, "path", queue_item->path);

  if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    {
      ret = snprintf(uri, sizeof(uri), "%s:%s:%d", "library", "track", queue_item->file_id);
      if (ret < sizeof(uri))
	json_object_object_add(item, "uri", json_object_new_string(uri));
    }
  else
    {
      safe_json_add_string(item, "uri", queue_item->path);
    }

  if (queue_item->artwork_url && net_is_http_or_https(queue_item->artwork_url))
    {
      // The queue item contains a valid http url for an artwork image, there is no need
      // for the client to request the image through the server artwork handler.
      // Directly pass the artwork url to the client.
      safe_json_add_string(item, "artwork_url", queue_item->artwork_url);
    }
  else if (queue_item->file_id > 0 && queue_item->file_id != DB_MEDIA_FILE_NON_PERSISTENT_ID)
    {
      if (queue_item->data_kind == DATA_KIND_FILE)
	{
	  // Queue item does not have a valid artwork url, construct artwork url to
	  // get the image through the httpd_artworkapi (uses the artwork handlers).
	  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/item/%d", queue_item->file_id);
	  if (ret < sizeof(artwork_url))
	    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));
	}
      else
	{
	  // Pipe and stream metadata can change if the queue version changes. Construct artwork url
	  // similar to non-pipe items, but append the queue version to the url to force
	  // clients to reload image if the queue version changes (additional metadata was found).
	  ret = snprintf(artwork_url, sizeof(artwork_url), "./artwork/item/%d?v=%d", queue_item->file_id, queue_item->queue_version);
	  if (ret < sizeof(artwork_url))
	    json_object_object_add(item, "artwork_url", json_object_new_string(artwork_url));
	}
    }

  safe_json_add_string(item, "type", queue_item->type);
  json_object_object_add(item, "bitrate", json_object_new_int(queue_item->bitrate));
  json_object_object_add(item, "samplerate", json_object_new_int(queue_item->samplerate));
  json_object_object_add(item, "channels", json_object_new_int(queue_item->channels));
  safe_json_add_string(item, "headers", queue_item->headers);

  return item;
}

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

static int
queue_tracks_add_byexpression(const char *param, char shuffle, uint32_t item_id, int pos, int limit, int *total_count, int *new_item_id)
{
  struct query_params query_params = { .type = Q_ITEMS, .sort = S_NAME };
  struct smartpl smartpl_expression = { 0 };
  char *expression;
  int ret;

  expression = safe_asprintf("\"query\" { %s }", param);
  ret = smartpl_query_parse_string(&smartpl_expression, expression);
  free(expression);

  if (ret < 0)
    return -1;

  query_params.filter = strdup(smartpl_expression.query_where);
  query_params.order = safe_strdup(smartpl_expression.order);
  query_params.limit = limit > 0 ? limit : smartpl_expression.limit;
  query_params.idx_type = query_params.limit > 0 ? I_FIRST : I_NONE;
  free_smartpl(&smartpl_expression, 1);

  ret = db_queue_add_by_query(&query_params, shuffle, item_id, pos, total_count, new_item_id);

  free_query_params(&query_params, 1);
  return ret;
}

/* Builds the queue-items JSON reply object (version/count/items) but does not
 * serialize it, so callers can add extra top-level keys (e.g. "skipped")
 * before sending it. Returns NULL on failure. Caller owns the returned object
 * and must jparse_free() it.
 */
static json_object *
build_reply_queue_tracks_add(int count, int new_item_id, char shuffle)
{
  json_object *reply = json_object_new_object();
  json_object *items = json_object_new_array();
  json_object *item;
  struct query_params query_params = { 0 };
  struct db_queue_item queue_item;
  int version = 0;
  int ret;

  db_admin_getint(&version, DB_ADMIN_QUEUE_VERSION);

  json_object_object_add(reply, "version", json_object_new_int(version));
  json_object_object_add(reply, "count", json_object_new_int(count));
  json_object_object_add(reply, "items", items);

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      if (queue_item.id < new_item_id)
	continue;

      item = queue_item_to_json(&queue_item, shuffle);
      if (!item)
	goto error;

      json_object_array_add(items, item);
    }

  db_queue_enum_end(&query_params);
  return reply;

 error:
  db_queue_enum_end(&query_params);
  jparse_free(reply);
  return NULL;
}

static int
create_reply_queue_tracks_add(struct evbuffer *evbuf, int count, int new_item_id, char shuffle)
{
  json_object *reply;
  int ret;

  reply = build_reply_queue_tracks_add(count, new_item_id, shuffle);
  if (!reply)
    return -1;

  ret = evbuffer_add_printf(evbuf, "%s", json_object_to_json_string(reply));

  jparse_free(reply);
  return (ret < 0) ? -1 : 0;
}

static int
jsonapi_reply_queue_tracks_add(struct httpd_request *hreq)
{
  const char *param_pos;
  const char *param_uris;
  const char *param_expression;
  const char *param_headers;
  const char *param;
  struct player_status status;
  int pos;
  int limit;
  bool shuffle;
  int total_count = 0;
  int new_item_id = 0;
  int ret = 0;


  param_pos = httpd_query_value_find(hreq->query, "position");
  if (param_pos)
    {
      if (safe_atoi32(param_pos, &pos) < 0)
        {
	  DPRINTF(E_LOG, L_WEB, "Invalid position parameter '%s'\n", param_pos);

	  return HTTP_BADREQUEST;
	}

      DPRINTF(E_DBG, L_WEB, "Add tracks starting at position %d\n", pos);
    }
  else
    pos = -1;

  param_uris = httpd_query_value_find(hreq->query, "uris");
  param_expression = httpd_query_value_find(hreq->query, "expression");
  param_headers = httpd_query_value_find(hreq->query, "headers");

  if (!param_uris && !param_expression)
    {
      DPRINTF(E_LOG, L_WEB, "Missing query parameter 'uris' or 'expression'\n");

      return HTTP_BADREQUEST;
    }

  // if query parameter "clear" is "true", stop playback and clear the queue before adding new queue items
  param = httpd_query_value_find(hreq->query, "clear");
  if (param && strcmp(param, "true") == 0)
    {
      player_playback_stop();
      db_queue_clear(0);
    }

  // if query parameter "shuffle" is present, update the shuffle state before adding new queue items
  param = httpd_query_value_find(hreq->query, "shuffle");
  if (param)
    {
      shuffle = (strcmp(param, "true") == 0);
      player_shuffle_set(shuffle);
    }

  player_get_status(&status);

  if (param_uris)
    {
      ret = queue_tracks_add_byuris(param_uris, param_headers, status.shuffle, status.item_id, pos, &total_count, &new_item_id);
    }
  else
    {
      // This overrides the value specified in query
      param = httpd_query_value_find(hreq->query, "limit");
      if (param && safe_atoi32(param, &limit) == 0)
	ret = queue_tracks_add_byexpression(param_expression, status.shuffle, status.item_id, pos, limit, &total_count, &new_item_id);
      else
	ret = queue_tracks_add_byexpression(param_expression, status.shuffle, status.item_id, pos, -1, &total_count, &new_item_id);
    }
  if (ret < 0)
    return HTTP_INTERNAL;

  ret = create_reply_queue_tracks_add(hreq->out_body, total_count, new_item_id, status.shuffle);
  if (ret < 0)
    return HTTP_INTERNAL;

  // If query parameter "playback" is "start", start playback after successfully adding new items
  param = httpd_query_value_find(hreq->query, "playback");
  if (param && strcmp(param, "start") == 0)
    {
      if ((param = httpd_query_value_find(hreq->query, "playback_from_position")))
	ret = (play_item_at_position(param) == HTTP_NOCONTENT) ? 0 : -1;
      else
	ret = player_playback_start();

      if (ret < 0)
	return HTTP_INTERNAL;
    }

  return HTTP_OK;
}

/* Each URL triggers a blocking yt-dlp resolve (up to ~20s) on the httpd
 * worker thread, so cap the batch to the same max as /api/youtube/search.
 */
#define YOUTUBE_QUEUE_MAX_URLS 25

/* POST /api/youtube/queue - resolves a batch of YouTube URLs and queues all
 * successfully-resolved ones in a single queue-add call. URLs that fail
 * validation or resolution are skipped (not fatal to the batch) and reported
 * back in the "skipped" array of the reply. If the batch exceeds
 * YOUTUBE_QUEUE_MAX_URLS, only the first YOUTUBE_QUEUE_MAX_URLS are processed
 * and the remainder are reported as skipped too.
 */
static int
jsonapi_reply_youtube_queue(struct httpd_request *hreq)
{
  json_object *request = NULL;
  json_object *urls = NULL;
  json_object *jurl;
  json_object *jreply = NULL;
  json_object *skipped_urls = NULL;
  struct player_status status;
  const char *url;
  char *title = NULL;
  char *stream_url = NULL;
  char *joined_uris = NULL;
  char *tmp;
  int total_urls;
  int nurls;
  int i;
  int total_count = 0;
  int new_item_id = 0;
  int ret;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse incoming request\n");
      return HTTP_BADREQUEST;
    }

  ret = jparse_array_from_obj(request, "urls", &urls);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Missing or invalid 'urls' array in YouTube queue post request\n");
      jparse_free(request);
      return HTTP_BADREQUEST;
    }

  CHECK_NULL(L_WEB, skipped_urls = json_object_new_array());

  total_urls = json_object_array_length(urls);
  nurls = total_urls;
  if (nurls > YOUTUBE_QUEUE_MAX_URLS)
    {
      DPRINTF(E_LOG, L_WEB, "YouTube queue request has %d URLs, capping to %d and skipping the rest\n", nurls, YOUTUBE_QUEUE_MAX_URLS);
      nurls = YOUTUBE_QUEUE_MAX_URLS;
    }

  for (i = 0; i < nurls; i++)
    {
      jurl = json_object_array_get_idx(urls, i);
      url = (jurl && json_object_get_type(jurl) == json_type_string) ? json_object_get_string(jurl) : NULL;

      if (!url || strlen(url) == 0 || !is_youtube_url(url))
	{
	  DPRINTF(E_LOG, L_WEB, "Skipping invalid YouTube URL in queue request: '%s'\n", url ? url : "(none)");
	  json_object_array_add(skipped_urls, json_object_new_string(url ? url : ""));
	  continue;
	}

      if (resolve_youtube_stream_url(url, &title, &stream_url) < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "Failed to resolve YouTube URL '%s' for queueing\n", url);
	  json_object_array_add(skipped_urls, json_object_new_string(url));
	  continue;
	}

      free(title);
      title = NULL;

      if (!joined_uris)
	joined_uris = stream_url;
      else
	{
	  tmp = safe_asprintf("%s,%s", joined_uris, stream_url);
	  free(joined_uris);
	  free(stream_url);
	  joined_uris = tmp;
	}
      stream_url = NULL;
    }

  /* Report the URLs beyond the cap as skipped too, without attempting to resolve them */
  for (; i < total_urls; i++)
    {
      jurl = json_object_array_get_idx(urls, i);
      url = (jurl && json_object_get_type(jurl) == json_type_string) ? json_object_get_string(jurl) : NULL;
      json_object_array_add(skipped_urls, json_object_new_string(url ? url : ""));
    }

  jparse_free(request);

  if (!joined_uris)
    {
      DPRINTF(E_LOG, L_WEB, "Could not resolve any of the given YouTube URLs for queueing\n");

      CHECK_NULL(L_WEB, jreply = json_object_new_object());
      safe_json_add_string(jreply, "error", "Could not resolve any of the given URLs");
      json_object_object_add(jreply, "skipped", skipped_urls);
      CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

      jparse_free(jreply);
      return HTTP_BADREQUEST;
    }

  player_get_status(&status);

  ret = queue_tracks_add_byuris(joined_uris, "Referer: https://www.youtube.com/", status.shuffle, status.item_id, -1, &total_count, &new_item_id);
  free(joined_uris);
  if (ret < 0)
    {
      jparse_free(skipped_urls);
      return HTTP_INTERNAL;
    }

  jreply = build_reply_queue_tracks_add(total_count, new_item_id, status.shuffle);
  if (!jreply)
    {
      jparse_free(skipped_urls);
      return HTTP_INTERNAL;
    }

  json_object_object_add(jreply, "skipped", skipped_urls);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));

  jparse_free(jreply);

  return HTTP_OK;
}

#undef YOUTUBE_QUEUE_MAX_URLS

static int
update_pos(uint32_t item_id, const char *new, char shuffle)
{
  uint32_t new_position;
  int ret;

  if (safe_atou32(new, &new_position) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item new_position '%s'\n", new);
      return HTTP_BADREQUEST;
    }

  ret = db_queue_move_byitemid(item_id, new_position, shuffle);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Moving item '%d' to new position %d failed\n", item_id, new_position);
      return HTTP_INTERNAL;
    }

  return HTTP_OK;
}

static inline void
update_str(bool *is_changed, char **str, const char *new)
{
  free(*str);
  *str = strdup(new);
  *is_changed = true;
}

static int
jsonapi_reply_queue_tracks_update(struct httpd_request *hreq)
{
  struct db_queue_item *queue_item;
  struct player_status status;
  uint32_t item_id = 0;
  const char *param;
  bool is_changed;
  int ret;

  player_get_status(&status);

  if (strcmp(hreq->path_parts[3], "now_playing") != 0)
    {
      ret = safe_atou32(hreq->path_parts[3], &item_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "No valid item id given: '%s'\n", hreq->path);
	  return HTTP_BADREQUEST;
	}
    }
  else
    item_id = status.item_id;

  queue_item = db_queue_fetch_byitemid(item_id);
  if (!queue_item)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given, or now_playing given but not playing: '%s'\n", hreq->path);
      return HTTP_BADREQUEST;
    }

  ret = HTTP_OK;
  is_changed = false;
  if ((param = httpd_query_value_find(hreq->query, "new_position")))
    ret = update_pos(item_id, param, status.shuffle);
  if ((param = httpd_query_value_find(hreq->query, "title")))
    update_str(&is_changed, &queue_item->title, param);
  if ((param = httpd_query_value_find(hreq->query, "album")))
    update_str(&is_changed, &queue_item->album, param);
  if ((param = httpd_query_value_find(hreq->query, "artist")))
    update_str(&is_changed, &queue_item->artist, param);
  if ((param = httpd_query_value_find(hreq->query, "album_artist")))
    update_str(&is_changed, &queue_item->album_artist, param);
  if ((param = httpd_query_value_find(hreq->query, "composer")))
    update_str(&is_changed, &queue_item->composer, param);
  if ((param = httpd_query_value_find(hreq->query, "genre")))
    update_str(&is_changed, &queue_item->genre, param);
  if ((param = httpd_query_value_find(hreq->query, "artwork_url")))
    update_str(&is_changed, &queue_item->artwork_url, param);
  if ((param = httpd_query_value_find(hreq->query, "headers")))
    update_str(&is_changed, &queue_item->headers, param);

  if (ret != HTTP_OK)
    return ret;

  if (is_changed)
    db_queue_item_update(queue_item);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue_tracks_delete(struct httpd_request *hreq)
{
  uint32_t item_id;
  uint32_t count;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &item_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid item id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  ret = db_queue_delete_byitemid(item_id);
  if (ret < 0)
    {
      return HTTP_INTERNAL;
    }

  db_queue_get_count(&count);
  if (count == 0)
    {
      player_playback_stop();
      db_queue_clear(0);
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue_clear(struct httpd_request *hreq)
{
  player_playback_stop();
  db_queue_clear(0);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_queue(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  uint32_t item_id;
  uint32_t count;
  int start_pos, end_pos;
  int version = 0;
  char etag[21];
  struct player_status status;
  struct db_queue_item queue_item;
  json_object *reply;
  json_object *items;
  json_object *item;
  int ret = 0;

  db_admin_getint(&version, DB_ADMIN_QUEUE_VERSION);
  db_queue_get_count(&count);

  snprintf(etag, sizeof(etag), "%d", version);
  if (httpd_request_etag_matches(hreq, etag))
    return HTTP_NOTMODIFIED;

  memset(&query_params, 0, sizeof(struct query_params));
  reply = json_object_new_object();

  json_object_object_add(reply, "version", json_object_new_int(version));
  json_object_object_add(reply, "count", json_object_new_int((int)count));

  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  player_get_status(&status);
  if (status.shuffle)
    query_params.sort = S_SHUFFLE_POS;

  param = httpd_query_value_find(hreq->query, "id");
  if (param && strcmp(param, "now_playing") == 0)
    {
      query_params.filter = db_mprintf("id = %d", status.item_id);
    }
  else if (param && safe_atou32(param, &item_id) == 0)
    {
      query_params.filter = db_mprintf("id = %d", item_id);
    }
  else
    {
      param = httpd_query_value_find(hreq->query, "start");
      if (param && safe_atoi32(param, &start_pos) == 0)
	{
	  param = httpd_query_value_find(hreq->query, "end");
	  if (!param || safe_atoi32(param, &end_pos) != 0)
	    {
	      end_pos = start_pos + 1;
	    }

	  if (query_params.sort == S_SHUFFLE_POS)
	    query_params.filter = db_mprintf("shuffle_pos >= %d AND shuffle_pos < %d", start_pos, end_pos);
	  else
	    query_params.filter = db_mprintf("pos >= %d AND pos < %d", start_pos, end_pos);
	}
    }

  ret = db_queue_enum_start(&query_params);
  if (ret < 0)
    goto db_start_error;

  while ((ret = db_queue_enum_fetch(&query_params, &queue_item)) == 0 && queue_item.id > 0)
    {
      item = queue_item_to_json(&queue_item, status.shuffle);
      if (!item)
	goto error;

      json_object_array_add(items, item);
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "outputs: Couldn't add outputs to response buffer.\n");

 error:
  db_queue_enum_end(&query_params);
 db_start_error:
  jparse_free(reply);
  free(query_params.filter);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_player_repeat(struct httpd_request *hreq)
{
  const char *param;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "single") == 0)
    {
      player_repeat_set(REPEAT_SONG);
    }
  else if (strcmp(param, "all") == 0)
    {
      player_repeat_set(REPEAT_ALL);
    }
  else if (strcmp(param, "off") == 0)
    {
      player_repeat_set(REPEAT_OFF);
    }

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_shuffle(struct httpd_request *hreq)
{
  const char *param;
  bool shuffle;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  shuffle = (strcmp(param, "true") == 0);
  player_shuffle_set(shuffle);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_player_consume(struct httpd_request *hreq)
{
  const char *param;
  bool consume;

  param = httpd_query_value_find(hreq->query, "state");
  if (!param)
    return HTTP_BADREQUEST;

  consume = (strcmp(param, "true") == 0);
  player_consume_set(consume);

  return HTTP_NOCONTENT;
}

static int
volume_set(int volume, int step)
{
  int new_volume;
  struct player_status status;
  int ret;

  new_volume = volume;

  if (step != 0)
    {
      // Calculate new volume from given step value
      player_get_status(&status);
      new_volume = status.volume + step;
    }

  // Make sure we are setting a correct value
  new_volume = new_volume > 100 ? 100 : new_volume;
  new_volume = new_volume < 0 ? 0 : new_volume;

  ret = player_volume_set(new_volume);
  return ret;
}

static int
output_volume_set(int volume, int step, uint64_t output_id)
{
  int new_volume;
  struct player_speaker_info speaker_info;
  int ret;

  new_volume = volume;

  if (step != 0)
    {
      // Calculate new output volume from the given step value
      ret = player_speaker_get_byid(&speaker_info, output_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "No output found for the given output id .\n");
	  return -1;
	}

      new_volume = speaker_info.absvol + step;
    }

  // Make sure we are setting a correct value
  new_volume = new_volume > 100 ? 100 : new_volume;
  new_volume = new_volume < 0 ? 0 : new_volume;

  ret = player_volume_setabs_speaker(output_id, new_volume);
  return ret;
}

static int
jsonapi_reply_player_volume(struct httpd_request *hreq)
{
  const char *param_volume;
  const char *param_step;
  const char *param;
  uint64_t output_id;
  int volume;
  int step;
  int ret;

  volume = 0;
  step = 0;

  // Parse and validate parameters
  param_volume = httpd_query_value_find(hreq->query, "volume");
  if (param_volume)
    {
      ret = safe_atoi32(param_volume, &volume);
      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  param_step = httpd_query_value_find(hreq->query, "step");
  if (param_step)
    {
      ret = safe_atoi32(param_step, &step);
      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  if ((!param_volume && !param_step)
      || (param_volume && param_step))
    {
      DPRINTF(E_LOG, L_WEB, "Invalid parameters for player/volume request. Either 'volume' or 'step' parameter required.\n");
      return HTTP_BADREQUEST;
    }

  param = httpd_query_value_find(hreq->query, "output_id");
  if (param)
    {
      // Update volume for individual output
      ret = safe_atou64(param, &output_id);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid value for parameter 'output_id'. Output id must be an integer (output_id='%s').\n", param);
	  return HTTP_BADREQUEST;
	}
      ret = output_volume_set(volume, step, output_id);
    }
  else
    {
      // Update master volume
      ret = volume_set(volume, step);
    }

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_library_artists(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_artists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_artist(struct httpd_request *hreq)
{
  const char *artist_id;
  json_object *reply;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  artist_id = hreq->path_parts[3];

  reply = fetch_artist(&notfound, artist_id);
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_artist_albums(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *artist_id;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  artist_id = hreq->path_parts[3];

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songartistid = %q)", artist_id);

  ret = fetch_albums(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add albums to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_albums(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_albums(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add albums to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album(struct httpd_request *hreq)
{
  const char *album_id;
  json_object *reply;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  album_id = hreq->path_parts[3];

  reply = fetch_album(&notfound, album_id);
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add artists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album_tracks(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *album_id;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  album_id = hreq->path_parts[3];

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_ITEMS;
  query_params.sort = S_ALBUM;
  query_params.filter = db_mprintf("(f.songalbumid = %q)", album_id);

  ret = fetch_tracks(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add tracks to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_album_tracks_put_byid(struct httpd_request *hreq)
{
  const char *param;
  int64_t album_id;;
  int ret;

  ret = safe_atoi64(hreq->path_parts[3], &album_id);
  if (ret < 0)
    return HTTP_INTERNAL;

  param = httpd_query_value_find(hreq->query, "play_count");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "increment") == 0)
    {
      db_file_inc_playcount_bysongalbumid(album_id, false);
    }
  else if (strcmp(param, "played") == 0)
    {
      db_file_inc_playcount_bysongalbumid(album_id, true);
    }
  else
    {
      DPRINTF(E_WARN, L_WEB, "Ignoring invalid play_count param '%s'\n", param);
      return HTTP_BADREQUEST;
    }

  return HTTP_OK;
}

static int
jsonapi_reply_library_tracks_get_byid(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *track_id;
  struct db_media_file_info dbmfi;
  json_object *reply = NULL;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  track_id = hreq->path_parts[3];

  memset(&query_params, 0, sizeof(struct query_params));

  query_params.type = Q_ITEMS;
  query_params.filter = db_mprintf("(f.id = %q)", track_id);

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  ret = db_query_fetch_file(&dbmfi, &query_params);
  if (ret < 0)
    goto error;
  else if (ret == 1)
    {
      DPRINTF(E_LOG, L_WEB, "Track with id '%s' not found.\n", track_id);
      ret = -1;
      notfound = true;
      goto error;
    }

  reply = track_to_json(&dbmfi);

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add track to response buffer.\n");

 error:
  db_query_end(&query_params);
  free(query_params.filter);
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_tracks_put(struct httpd_request *hreq)
{
  json_object *request = NULL;
  json_object *tracks;
  json_object *track = NULL;
  int ret;
  int err;
  int32_t track_id;
  int i;
  int j;

  request = jparse_obj_from_evbuffer(hreq->in_body);
  if (!request)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to read json tracks request\n");
      err = HTTP_BADREQUEST;
      goto error;
    }

  ret = jparse_array_from_obj(request, "tracks", &tracks);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Failed to parse json tracks request\n");
      err = HTTP_BADREQUEST;
      goto error;
    }

  db_transaction_begin();
  i = 0;
  while ((track = json_object_array_get_idx(tracks, i)))
    {
      track_id = jparse_int_from_obj(track, "id");
      if (track_id == 0)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid or missing track id in json tracks request\n");
	  err = HTTP_BADREQUEST;
	  goto error;
	}

      if (!db_file_id_exists(track_id))
	{
	  DPRINTF(E_LOG, L_WEB, "Unknown track_id %d in json tracks request\n", track_id);
	  err = HTTP_NOTFOUND;
	  goto error;
	}

      for (j = 0; j < ARRAY_SIZE(track_attribs); j++)
	{
	  if (!jparse_contains_key(track, track_attribs[j].name, json_type_int))
	    continue;

	  ret = jparse_int_from_obj(track, track_attribs[j].name);
	  if (ret < 0)
	    continue;

	  // async, so no error check
	  library_item_attrib_save(track_id, track_attribs[j].type, ret);
	}

      i++;
    }

  jparse_free(request);
  db_transaction_end();
  return HTTP_OK;

 error:
  jparse_free(request);
  if (track)
    db_transaction_rollback();
  return err;
}

static int
jsonapi_reply_library_tracks_put_byid(struct httpd_request *hreq)
{
  int track_id;
  const char *param;
  uint32_t val;
  int ret;
  int i;

  ret = safe_atoi32(hreq->path_parts[3], &track_id);
  if (ret < 0 || !db_file_id_exists(track_id))
    {
      DPRINTF(E_WARN, L_WEB, "Invalid or unknown track id in request '%s'\n", hreq->path);
      return HTTP_NOTFOUND;
    }

  for (i = 0; i < ARRAY_SIZE(track_attribs); i++)
    {
      param = httpd_query_value_find(hreq->query, track_attribs[i].name);
      if (!param)
	continue;

      // Special cases
      if (track_attribs[i].type == LIBRARY_ATTRIB_PLAY_COUNT && strcmp(param, "increment") == 0)
	{
	  db_file_inc_playcount(track_id);
	  continue;
	}
      if (track_attribs[i].type == LIBRARY_ATTRIB_PLAY_COUNT && strcmp(param, "reset") == 0)
	{
	  db_file_reset_playskip_count(track_id);
	  continue;
	}

      ret = safe_atou32(param, &val);
      if (ret < 0)
	{
	  DPRINTF(E_WARN, L_WEB, "Invalid %s value '%s' for track '%d'.\n", track_attribs[i].name, param, track_id);
	  return HTTP_BADREQUEST;
	}

      library_item_attrib_save(track_id, track_attribs[i].type, val);
    }

  return HTTP_OK;
}

static int
jsonapi_reply_library_track_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  char *path;
  const char *track_id;
  int id;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;

  track_id = hreq->path_parts[3];
  if (safe_atoi32(track_id, &id) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Error converting track id '%s' to int.\n", track_id);
      return HTTP_INTERNAL;
    }

  path = db_file_path_byid(id);
  if (!path)
    {
      DPRINTF(E_WARN, L_WEB, "No file path found for track with id '%s' not found.\n", track_id);
      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_FIND_PL;
  query_params.filter = db_mprintf("filepath = '%q'", path);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "track playlists: Couldn't add playlists to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);
  free(path);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("(f.type = %d OR f.type = %d OR f.type = %d)", PL_PLAIN, PL_SMART, PL_RSS);

  ret = fetch_playlists(&query_params, items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add playlists to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_get(struct httpd_request *hreq)
{
  uint32_t playlist_id;
  json_object *reply = NULL;
  int ret = 0;
  bool notfound = false;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  ret = safe_atou32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not parse playlist id to integer\n");
      goto error;
    }

  if (playlist_id == 0)
    {
      reply = json_object_new_object();
      json_object_object_add(reply, "id", json_object_new_int(0));
      json_object_object_add(reply, "name", json_object_new_string("Playlists"));
      json_object_object_add(reply, "type", json_object_new_string(db_pl_type_label(PL_FOLDER)));
      json_object_object_add(reply, "smart_playlist", json_object_new_boolean(false));
      json_object_object_add(reply, "folder", json_object_new_boolean(true));
    }
  else
    {
      reply = fetch_playlist(&notfound, playlist_id);
    }

  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add playlist to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return notfound ? HTTP_NOTFOUND : HTTP_INTERNAL;

  return HTTP_OK;
}

static int
playlist_attrib_query_limit_set(int playlist_id, const char *param)
{
  struct playlist_info *pli;
  int query_limit;
  int ret;

  ret = safe_atoi32(param, &query_limit);
  if (ret < 0)
    return -1;

  pli = db_pl_fetch_byid(playlist_id);
  if (!pli)
    return -1;

  pli->query_limit = query_limit;

  ret = db_pl_update(pli);

  free_pli(pli, 0);

  return ret;
}

static int
jsonapi_reply_library_playlist_put(struct httpd_request *hreq)
{
  uint32_t playlist_id;
  const char *param;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Could not parse playlist id to integer\n");
      return HTTP_BADREQUEST;
    }

  if ((param = httpd_query_value_find(hreq->query, "query_limit")))
    ret = playlist_attrib_query_limit_set(playlist_id, param);
  else
    ret = -1;

  if (ret < 0)
    return HTTP_BADREQUEST;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_tracks(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int playlist_id;
  int total;
  int ret = 0;

  // Due to smart playlists possibly changing their tracks between rescans, disable caching in clients
  httpd_response_not_cachable(hreq);

  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PLITEMS;
  query_params.id = playlist_id;

  ret = fetch_tracks(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_delete(struct httpd_request *hreq)
{
  uint32_t pl_id;
  int ret;

  ret = safe_atou32(hreq->path_parts[3], &pl_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  library_playlist_remove_byid(pl_id);

  return HTTP_NOCONTENT;
}

static int
jsonapi_reply_library_playlist_playlists(struct httpd_request *hreq)
{
  struct query_params query_params;
  json_object *reply;
  json_object *items;
  int playlist_id;
  int total;
  int ret = 0;

  if (!is_modified(hreq, DB_ADMIN_DB_MODIFIED))
    return HTTP_NOTMODIFIED;


  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_WEB, "No valid playlist id given '%s'\n", hreq->path);

      return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("f.parent_id = %d AND (f.type = %d OR f.type = %d OR f.type = %d OR f.type = %d)",
				   playlist_id, PL_PLAIN, PL_SMART, PL_RSS, PL_FOLDER);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  free_query_params(&query_params, 1);
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_playlist_tracks_put_byid(struct httpd_request *hreq)
{
  const char *param;
  int playlist_id;
  int ret;

  ret = safe_atoi32(hreq->path_parts[3], &playlist_id);
  if (ret < 0)
    return HTTP_INTERNAL;

  param = httpd_query_value_find(hreq->query, "play_count");
  if (!param)
    return HTTP_BADREQUEST;

  if (strcmp(param, "increment") == 0)
    {
      db_file_inc_playcount_byplid(playlist_id, false);
    }
  else if (strcmp(param, "played") == 0)
    {
      db_file_inc_playcount_byplid(playlist_id, true);
    }
  else
    {
      DPRINTF(E_WARN, L_WEB, "Ignoring invalid play_count param '%s'\n", param);
      return HTTP_BADREQUEST;
    }

  return HTTP_OK;
}

static int
jsonapi_reply_queue_save(struct httpd_request *hreq)
{
  const char *param;
  char buf[PATH_MAX+7];
  char *playlist_name = NULL;
  int ret = 0;

  if ((param = httpd_query_value_find(hreq->query, "name")) == NULL)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid argument, missing 'name'\n");
      return HTTP_BADREQUEST;
    }

  if (!allow_modifying_stored_playlists)
    {
      DPRINTF(E_LOG, L_WEB, "Modifying stored playlists is not enabled in the config file\n");
      return 403;
    }

  if (access(default_playlist_directory, W_OK) < 0)
    {
      DPRINTF(E_LOG, L_WEB, "Invalid playlist save directory '%s'\n", default_playlist_directory);
      return 403;
   }

  playlist_name = atrim(param);

  if (strlen(playlist_name) < 1) {
      free(playlist_name);

      DPRINTF(E_LOG, L_WEB, "Empty playlist name parameter is not allowed\n");
      return HTTP_BADREQUEST;
  }

  snprintf(buf, sizeof(buf), "/file:%s/%s", default_playlist_directory, playlist_name);
  free(playlist_name);

  ret = library_queue_save(buf);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_browse(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *param;
  const char *browse_type;
  enum media_kind media_kind;
  json_object *reply;
  json_object *items;
  int total;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  browse_type = hreq->path_parts[2];
  DPRINTF(E_DBG, L_WEB, "Browse query with type '%s'\n", browse_type);

  media_kind = 0;
  param = httpd_query_value_find(hreq->query, "media_kind");
  if (param)
    {
      media_kind = db_media_kind_enum(param);
      if (!media_kind)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param);
	  return HTTP_BADREQUEST;
	}
    }

  reply = json_object_new_object();
  items = json_object_new_array();
  json_object_object_add(reply, "items", items);

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  if (strcmp(browse_type, "genres") == 0)
    {
      query_params.type = Q_BROWSE_GENRES;
      query_params.sort = S_GENRE;
      query_params.idx_type = I_NONE;
    }
  else if (strcmp(browse_type, "composers") == 0)
    {
      query_params.type = Q_BROWSE_COMPOSERS;
      query_params.sort = S_COMPOSER;
      query_params.idx_type = I_NONE;
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid browse type '%s'\n", browse_type);
      goto error;
    }

  if (media_kind)
    query_params.filter = db_mprintf("(f.media_kind = %d)", media_kind);

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto error;

  json_object_object_add(reply, "total", json_object_new_int(total));
  json_object_object_add(reply, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(reply, "limit", json_object_new_int(query_params.limit));

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add browse items to response buffer.\n");

 error:
  jparse_free(reply);
  free_query_params(&query_params, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_browseitem(struct httpd_request *hreq)
{
  struct query_params query_params;
  const char *browse_type;
  const char *item_name;
  struct db_browse_info dbbi;
  json_object *reply;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  browse_type = hreq->path_parts[2];
  item_name = hreq->path_parts[3];
  DPRINTF(E_DBG, L_WEB, "Browse item query with type '%s'\n", browse_type);

  reply = json_object_new_object();

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  if (strcmp(browse_type, "genres") == 0)
    {
      query_params.type = Q_BROWSE_GENRES;
      query_params.sort = S_GENRE;
      query_params.idx_type = I_NONE;
      query_params.filter = db_mprintf("(f.genre = %Q)", item_name);
    }
  else if (strcmp(browse_type, "composers") == 0)
    {
      query_params.type = Q_BROWSE_COMPOSERS;
      query_params.sort = S_COMPOSER;
      query_params.idx_type = I_NONE;
      query_params.filter = db_mprintf("(f.composer = %Q)", item_name);
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "Invalid browse type '%s'\n", browse_type);
      goto error;
    }

  ret = db_query_start(&query_params);
  if (ret < 0)
    goto error;

  if ((ret = db_query_fetch_browse(&dbbi, &query_params)) == 0)
    {
      reply = browse_info_to_json(&dbbi);
    }
  if (!reply)
    {
      ret = -1;
      goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add browse item to response buffer.\n");

 error:
  db_query_end(&query_params);
  jparse_free(reply);
  free_query_params(&query_params, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_count(struct httpd_request *hreq)
{
  const char *param_expression;
  char *expression;
  struct smartpl smartpl_expression;
  struct query_params qp;
  struct filecount_info fci;
  json_object *jreply;
  int ret;

  if (!is_modified(hreq, DB_ADMIN_DB_UPDATE))
    return HTTP_NOTMODIFIED;

  memset(&qp, 0, sizeof(struct query_params));
  qp.type = Q_COUNT_ITEMS;

  param_expression = httpd_query_value_find(hreq->query, "expression");
  if (param_expression)
    {
      memset(&smartpl_expression, 0, sizeof(struct smartpl));
      expression = safe_asprintf("\"query\" { %s }", param_expression);
      ret = smartpl_query_parse_string(&smartpl_expression, expression);
      free(expression);

      if (ret < 0)
	return HTTP_BADREQUEST;

      qp.filter = strdup(smartpl_expression.query_where);
      free_smartpl(&smartpl_expression, 1);
    }

  CHECK_NULL(L_WEB, jreply = json_object_new_object());

  ret = db_filecount_get(&fci, &qp);
  if (ret == 0)
    {
      json_object_object_add(jreply, "tracks", json_object_new_int(fci.count));
      json_object_object_add(jreply, "artists", json_object_new_int(fci.artist_count));
      json_object_object_add(jreply, "albums", json_object_new_int(fci.album_count));
      json_object_object_add(jreply, "db_playtime", json_object_new_int64((fci.length / 1000)));
      json_object_object_add(jreply, "file_size", json_object_new_int64((fci.file_size)));
    }
  else
    {
      DPRINTF(E_LOG, L_WEB, "library: failed to get count info\n");
    }

  free(qp.filter);

  CHECK_ERRNO(L_WEB, evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(jreply)));
  jparse_free(jreply);

  return HTTP_OK;
}

static int
jsonapi_reply_library_files(struct httpd_request *hreq)
{
  const char *param;
  int directory_id;
  json_object *reply;
  json_object *directories;
  struct query_params query_params;
  json_object *tracks;
  json_object *tracks_items;
  json_object *playlists;
  json_object *playlists_items;
  int total;
  int ret;

  param = httpd_query_value_find(hreq->query, "directory");

  directory_id = DIR_FILE;
  if (param)
    {
      directory_id = db_directory_id_bypath(param);
      if (directory_id <= 0)
	return HTTP_INTERNAL;
    }

  reply = json_object_new_object();

  // Add sub directories to response
  directories = json_object_new_array();
  json_object_object_add(reply, "directories", directories);

  ret = fetch_directories(directory_id, directories);
  if (ret < 0)
    {
      goto error;
    }

  // Add tracks to response
  tracks = json_object_new_object();
  json_object_object_add(reply, "tracks", tracks);
  tracks_items = json_object_new_array();
  json_object_object_add(tracks, "items", tracks_items);
  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_ITEMS;
  query_params.sort = S_VPATH;
  query_params.filter = db_mprintf("(f.directory_id = %d)", directory_id);

  ret = fetch_tracks(&query_params, tracks_items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(tracks, "total", json_object_new_int(total));
  json_object_object_add(tracks, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(tracks, "limit", json_object_new_int(query_params.limit));

  // Add playlists
  playlists = json_object_new_object();
  json_object_object_add(reply, "playlists", playlists);
  playlists_items = json_object_new_array();
  json_object_object_add(playlists, "items", playlists_items);
  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto error;

  query_params.type = Q_PL;
  query_params.sort = S_VPATH;
  query_params.filter = db_mprintf("(f.directory_id = %d)", directory_id);

  ret = fetch_playlists(&query_params, playlists_items, &total);
  free(query_params.filter);

  if (ret < 0)
    goto error;

  json_object_object_add(playlists, "total", json_object_new_int(total));
  json_object_object_add(playlists, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(playlists, "limit", json_object_new_int(query_params.limit));

  // Build JSON response
  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "browse: Couldn't add directories to response buffer.\n");

 error:
  jparse_free(reply);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_add(struct httpd_request *hreq)
{
  const char *url;
  int ret;

  url = httpd_query_value_find(hreq->query, "url");
  if (!url)
    {
      DPRINTF(E_LOG, L_WEB, "Missing URL parameter for library add\n");
      return HTTP_BADREQUEST;
    }

  ret = library_item_add(url);
  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
search_tracks(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  type = json_object_new_object();
  json_object_object_add(reply, "tracks", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_ITEMS;
  query_params.sort = S_NAME;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.title LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.title LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_tracks(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_artists(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "artists", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_GROUP_ARTISTS;
  query_params.sort = S_ARTIST;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.album_artist LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.album_artist LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_artists(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_albums(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "albums", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_GROUP_ALBUMS;
  query_params.sort = S_ALBUM;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.album LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.album LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_albums(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_composers(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "composers", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_BROWSE_COMPOSERS;
  query_params.sort = S_COMPOSER;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.composer LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.composer LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_genres(json_object *reply, struct httpd_request *hreq, const char *param_query, struct smartpl *smartpl_expression, enum media_kind media_kind)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "genres", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_BROWSE_GENRES;
  query_params.sort = S_GENRE;

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  if (param_query)
    {
      if (media_kind)
	query_params.filter = db_mprintf("(f.genre LIKE '%%%q%%' AND f.media_kind = %d)", param_query, media_kind);
      else
	query_params.filter = db_mprintf("(f.genre LIKE '%%%q%%')", param_query);
    }
  else
    {
      query_params.filter = strdup(smartpl_expression->query_where);
      query_params.having = safe_strdup(smartpl_expression->having);
      query_params.order = safe_strdup(smartpl_expression->order);

      if (smartpl_expression->limit > 0)
	{
	  query_params.idx_type = I_SUB;
	  query_params.limit = smartpl_expression->limit;
	  query_params.offset = 0;
	}
    }

  ret = fetch_browse_info(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
search_playlists(json_object *reply, struct httpd_request *hreq, const char *param_query)
{
  json_object *type;
  json_object *items;
  struct query_params query_params;
  int total;
  int ret;

  if (!param_query)
    return 0;

  memset(&query_params, 0, sizeof(struct query_params));

  ret = query_params_limit_set(&query_params, hreq);
  if (ret < 0)
    goto out;

  type = json_object_new_object();
  json_object_object_add(reply, "playlists", type);
  items = json_object_new_array();
  json_object_object_add(type, "items", items);

  query_params.type = Q_PL;
  query_params.sort = S_PLAYLIST;
  query_params.filter = db_mprintf("((f.type = %d OR f.type = %d OR f.type = %d) AND f.title LIKE '%%%q%%')", PL_PLAIN, PL_SMART, PL_RSS, param_query);

  ret = fetch_playlists(&query_params, items, &total);
  if (ret < 0)
    goto out;

  json_object_object_add(type, "total", json_object_new_int(total));
  json_object_object_add(type, "offset", json_object_new_int(query_params.offset));
  json_object_object_add(type, "limit", json_object_new_int(query_params.limit));

 out:
  free_query_params(&query_params, 1);

  return ret;
}

static int
jsonapi_reply_search(struct httpd_request *hreq)
{
  const char *param_type;
  const char *param_query;
  const char *param_expression;
  const char *param_media_kind;
  enum media_kind media_kind;
  char *expression;
  struct smartpl smartpl_expression;
  json_object *reply;
  int ret = 0;

  reply = NULL;

  param_type = httpd_query_value_find(hreq->query, "type");
  param_query = httpd_query_value_find(hreq->query, "query");
  param_expression = httpd_query_value_find(hreq->query, "expression");

  if (!param_type || (!param_query && !param_expression))
    {
      DPRINTF(E_LOG, L_WEB, "Missing request parameter\n");

      return HTTP_BADREQUEST;
    }

  media_kind = 0;
  param_media_kind = httpd_query_value_find(hreq->query, "media_kind");
  if (param_media_kind)
    {
      media_kind = db_media_kind_enum(param_media_kind);
      if (!media_kind)
      {
	DPRINTF(E_LOG, L_WEB, "Invalid media kind '%s'\n", param_media_kind);
	return HTTP_BADREQUEST;
      }
    }

  memset(&smartpl_expression, 0, sizeof(struct smartpl));

  if (param_expression)
    {
      expression = safe_asprintf("\"query\" { %s }", param_expression);

      ret = smartpl_query_parse_string(&smartpl_expression, expression);
      free(expression);

      if (ret < 0)
	return HTTP_BADREQUEST;
    }

  reply = json_object_new_object();

  if (strstr(param_type, "track"))
    {
      ret = search_tracks(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "artist"))
    {
      ret = search_artists(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "album"))
    {
      ret = search_albums(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
	goto error;
    }

  if (strstr(param_type, "composer"))
    {
      ret = search_composers(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
        goto error;
    }

  if (strstr(param_type, "genre"))
    {
      ret = search_genres(reply, hreq, param_query, &smartpl_expression, media_kind);
      if (ret < 0)
        goto error;
    }

  if (strstr(param_type, "playlist") && param_query)
    {
      ret = search_playlists(reply, hreq, param_query);
      if (ret < 0)
	goto error;
    }

  ret = evbuffer_add_printf(hreq->out_body, "%s", json_object_to_json_string(reply));
  if (ret < 0)
    DPRINTF(E_LOG, L_WEB, "playlist tracks: Couldn't add tracks to response buffer.\n");

 error:
  jparse_free(reply);
  free_smartpl(&smartpl_expression, 1);

  if (ret < 0)
    return HTTP_INTERNAL;

  return HTTP_OK;
}

static int
jsonapi_reply_library_backup(struct httpd_request *hreq)
{
  int ret;
  ret = db_backup();

  if (ret < 0)
  {
    if (ret == -2)
      return HTTP_SERVUNAVAIL;  // not enabled by config

    return HTTP_INTERNAL;
  }

  return HTTP_OK;
}


static struct httpd_uri_map adm_handlers[] =
  {
    { HTTPD_METHOD_GET,    "^/api/config$",                                jsonapi_reply_config },
    { HTTPD_METHOD_GET,    "^/api/settings$",                              jsonapi_reply_settings_get },
    { HTTPD_METHOD_GET,    "^/api/settings/[A-Za-z0-9_]+$",                jsonapi_reply_settings_category_get },
    { HTTPD_METHOD_GET,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_get },
    { HTTPD_METHOD_PUT,    "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_put },
    { HTTPD_METHOD_DELETE, "^/api/settings/[A-Za-z0-9_]+/[A-Za-z0-9_]+$",  jsonapi_reply_settings_option_delete },
    { HTTPD_METHOD_GET,    "^/api/library$",                               jsonapi_reply_library },
    { HTTPD_METHOD_GET |
      HTTPD_METHOD_PUT,    "^/api/update$",                                jsonapi_reply_update },
    { HTTPD_METHOD_PUT,    "^/api/rescan$",                                jsonapi_reply_meta_rescan },
    { HTTPD_METHOD_GET,    "^/api/spotify-logout$",                        jsonapi_reply_spotify_logout },
    { HTTPD_METHOD_GET,    "^/api/spotify$",                               jsonapi_reply_spotify },
    { HTTPD_METHOD_GET,    "^/api/youtube$",                                jsonapi_reply_youtube },
    { HTTPD_METHOD_POST,   "^/api/youtube/resolve$",                       jsonapi_reply_youtube_resolve },
    { HTTPD_METHOD_POST,   "^/api/youtube/search$",                       jsonapi_reply_youtube_search },
    { HTTPD_METHOD_POST,   "^/api/youtube/queue$",                        jsonapi_reply_youtube_queue },
    { HTTPD_METHOD_POST,   "^/api/youtube/resolve-playlist$",             jsonapi_reply_youtube_resolve_playlist },
    { HTTPD_METHOD_GET,    "^/api/pairing$",                               jsonapi_reply_pairing_get },
    { HTTPD_METHOD_POST,   "^/api/pairing$",                               jsonapi_reply_pairing_pair },
    { HTTPD_METHOD_POST,   "^/api/lastfm-login$",                          jsonapi_reply_lastfm_login },
    { HTTPD_METHOD_GET,    "^/api/lastfm-logout$",                         jsonapi_reply_lastfm_logout },
    { HTTPD_METHOD_GET,    "^/api/lastfm$",                                jsonapi_reply_lastfm },
    { HTTPD_METHOD_POST,   "^/api/verification$",                          jsonapi_reply_verification },

    { HTTPD_METHOD_GET,    "^/api/outputs$",                               jsonapi_reply_outputs },
    { HTTPD_METHOD_PUT,    "^/api/outputs/set$",                           jsonapi_reply_outputs_set },
    { HTTPD_METHOD_POST,   "^/api/select-outputs$",                        jsonapi_reply_outputs_set }, // deprecated: use "/api/outputs/set"
    { HTTPD_METHOD_GET,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_get_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/[[:digit:]]+$",                  jsonapi_reply_outputs_put_byid },
    { HTTPD_METHOD_PUT,    "^/api/outputs/[[:digit:]]+/toggle$",           jsonapi_reply_outputs_toggle_byid },

    { HTTPD_METHOD_GET,    "^/api/player$",                                jsonapi_reply_player },
    { HTTPD_METHOD_PUT,    "^/api/player/play$",                           jsonapi_reply_player_play },
    { HTTPD_METHOD_PUT,    "^/api/player/pause$",                          jsonapi_reply_player_pause },
    { HTTPD_METHOD_PUT,    "^/api/player/stop$",                           jsonapi_reply_player_stop },
    { HTTPD_METHOD_PUT,    "^/api/player/toggle$",                         jsonapi_reply_player_toggle },
    { HTTPD_METHOD_PUT,    "^/api/player/next$",                           jsonapi_reply_player_next },
    { HTTPD_METHOD_PUT,    "^/api/player/previous$",                       jsonapi_reply_player_previous },
    { HTTPD_METHOD_PUT,    "^/api/player/shuffle$",                        jsonapi_reply_player_shuffle },
    { HTTPD_METHOD_PUT,    "^/api/player/repeat$",                         jsonapi_reply_player_repeat },
    { HTTPD_METHOD_PUT,    "^/api/player/consume$",                        jsonapi_reply_player_consume },
    { HTTPD_METHOD_PUT,    "^/api/player/volume$",                         jsonapi_reply_player_volume },
    { HTTPD_METHOD_PUT,    "^/api/player/seek$",                           jsonapi_reply_player_seek },

    { HTTPD_METHOD_GET,    "^/api/queue$",                                 jsonapi_reply_queue },
    { HTTPD_METHOD_PUT,    "^/api/queue/clear$",                           jsonapi_reply_queue_clear },
    { HTTPD_METHOD_POST,   "^/api/queue/items/add$",                       jsonapi_reply_queue_tracks_add },
    { HTTPD_METHOD_PUT,    "^/api/queue/items/[[:digit:]]+$",              jsonapi_reply_queue_tracks_update },
    { HTTPD_METHOD_PUT,    "^/api/queue/items/now_playing$",               jsonapi_reply_queue_tracks_update },
    { HTTPD_METHOD_DELETE, "^/api/queue/items/[[:digit:]]+$",              jsonapi_reply_queue_tracks_delete },
    { HTTPD_METHOD_POST,   "^/api/queue/save$",                            jsonapi_reply_queue_save},

    { HTTPD_METHOD_GET,    "^/api/library/playlists$",                     jsonapi_reply_library_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_get },
    { HTTPD_METHOD_PUT,    "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_put },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+/tracks$", jsonapi_reply_library_playlist_tracks },
    { HTTPD_METHOD_PUT,    "^/api/library/playlists/[[:digit:]]+/tracks",  jsonapi_reply_library_playlist_tracks_put_byid},
//    { HTTPD_METHOD_POST,   "^/api/library/playlists/[[:digit:]]+/tracks$", jsonapi_reply_library_playlists_tracks },
    { HTTPD_METHOD_DELETE, "^/api/library/playlists/[[:digit:]]+$",        jsonapi_reply_library_playlist_delete },
    { HTTPD_METHOD_GET,    "^/api/library/playlists/[[:digit:]]+/playlists", jsonapi_reply_library_playlist_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/artists$",                       jsonapi_reply_library_artists },
    { HTTPD_METHOD_GET,    "^/api/library/artists/[[:digit:]]+$",          jsonapi_reply_library_artist },
    { HTTPD_METHOD_GET,    "^/api/library/artists/[[:digit:]]+/albums$",   jsonapi_reply_library_artist_albums },
    { HTTPD_METHOD_GET,    "^/api/library/albums$",                        jsonapi_reply_library_albums },
    { HTTPD_METHOD_GET,    "^/api/library/albums/[[:digit:]]+$",           jsonapi_reply_library_album },
    { HTTPD_METHOD_GET,    "^/api/library/albums/[[:digit:]]+/tracks$",    jsonapi_reply_library_album_tracks },
    { HTTPD_METHOD_PUT,    "^/api/library/albums/[[:digit:]]+/tracks$",    jsonapi_reply_library_album_tracks_put_byid },
    { HTTPD_METHOD_PUT,    "^/api/library/tracks$",                        jsonapi_reply_library_tracks_put },
    { HTTPD_METHOD_GET,    "^/api/library/tracks/[[:digit:]]+$",           jsonapi_reply_library_tracks_get_byid },
    { HTTPD_METHOD_PUT,    "^/api/library/tracks/[[:digit:]]+$",           jsonapi_reply_library_tracks_put_byid },
    { HTTPD_METHOD_GET,    "^/api/library/tracks/[[:digit:]]+/playlists$", jsonapi_reply_library_track_playlists },
    { HTTPD_METHOD_GET,    "^/api/library/(genres|composers)$",            jsonapi_reply_library_browse },
    { HTTPD_METHOD_GET,    "^/api/library/(genres|composers)/.*$",         jsonapi_reply_library_browseitem },
    { HTTPD_METHOD_GET,    "^/api/library/count$",                         jsonapi_reply_library_count },
    { HTTPD_METHOD_GET,    "^/api/library/files$",                         jsonapi_reply_library_files },
    { HTTPD_METHOD_POST,   "^/api/library/add$",                           jsonapi_reply_library_add },
    { HTTPD_METHOD_PUT,    "^/api/library/backup$",                        jsonapi_reply_library_backup },

    { HTTPD_METHOD_GET,    "^/api/search$",                                jsonapi_reply_search },

    { HTTPD_METHOD_GET,    "^/api/listenbrainz$",                          jsonapi_reply_listenbrainz },
    { HTTPD_METHOD_POST,   "^/api/listenbrainz/token$",                    jsonapi_reply_listenbrainz_token_add },
    { HTTPD_METHOD_DELETE, "^/api/listenbrainz/token$",                    jsonapi_reply_listenbrainz_token_delete },

    { 0, NULL, NULL }
  };


/* ------------------------------- JSON API --------------------------------- */

static void
jsonapi_request(struct httpd_request *hreq)
{
  int status_code;

  if (!httpd_request_is_authorized(hreq))
    {
      return;
    }

  if (!hreq->handler)
    {
      DPRINTF(E_LOG, L_WEB, "Unrecognized JSON API request: '%s'\n", hreq->uri);
      httpd_send_error(hreq, HTTP_BADREQUEST, "Bad Request");
      return;
    }

  status_code = hreq->handler(hreq);

  if (status_code >= 400)
    DPRINTF(E_LOG, L_WEB, "JSON api request failed with error code %d (%s)\n", status_code, hreq->uri);

  switch (status_code)
    {
      case HTTP_OK:                  /* 200 OK */
	httpd_header_add(hreq->out_headers, "Content-Type", "application/json");
	httpd_send_reply(hreq, status_code, "OK", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOCONTENT:           /* 204 No Content */
	httpd_send_reply(hreq, status_code, "No Content", HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_NOTMODIFIED:         /* 304 Not Modified */
	httpd_send_reply(hreq, HTTP_NOTMODIFIED, NULL, HTTPD_SEND_NO_GZIP);
	break;
      case HTTP_BADREQUEST:          /* 400 Bad Request */
	httpd_send_error(hreq, status_code, "Bad Request");
	break;
      case 403:
	httpd_send_error(hreq, status_code, "Forbidden");
	break;
      case HTTP_NOTFOUND:            /* 404 Not Found */
	httpd_send_error(hreq, status_code, "Not Found");
	break;
      case HTTP_SERVUNAVAIL:            /* 503 */
        httpd_send_error(hreq, status_code, "Service Unavailable");
        break;
      case HTTP_INTERNAL:            /* 500 Internal Server Error */
      default:
	httpd_send_error(hreq, HTTP_INTERNAL, "Internal Server Error");
	break;
    }
}

static int
jsonapi_init(void)
{
  char *temp_path;

  default_playlist_directory = NULL;
  allow_modifying_stored_playlists = cfg_getbool(cfg_getsec(cfg, "library"), "allow_modifying_stored_playlists");
  if (allow_modifying_stored_playlists)
    {
      temp_path = cfg_getstr(cfg_getsec(cfg, "library"), "default_playlist_directory");
      if (temp_path)
	{
	  // The path in the conf file may have a trailing slash character. Return the realpath like it is done for the library directories.
	  default_playlist_directory = realpath(temp_path, NULL);
	  if (default_playlist_directory)
	    {
	      if (access(default_playlist_directory, W_OK) < 0)
	        DPRINTF(E_WARN, L_WEB, "Non-writable playlist save directory '%s'\n", default_playlist_directory);
	    }
	}

      if (!default_playlist_directory)
	{
	  DPRINTF(E_LOG, L_WEB, "Invalid playlist save directory, disabling modifying stored playlists\n");
	  allow_modifying_stored_playlists = false;
	}
     }

  return 0;
}

static void
jsonapi_deinit(void)
{
  free(default_playlist_directory);
}

struct httpd_module httpd_jsonapi =
{
  .name = "JSON API",
  .type = MODULE_JSONAPI,
  .logdomain = L_WEB,
  .subpaths = { "/api/", NULL },
  .handlers = adm_handlers,
  .init = jsonapi_init,
  .deinit = jsonapi_deinit,
  .request = jsonapi_request,
};
