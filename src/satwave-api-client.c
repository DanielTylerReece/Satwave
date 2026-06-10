#include "satwave-api-client.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/*
 * SiriusXM Edge Gateway API client.
 *
 * Channel listing:  GET  /relationship/v1/container/all-channels
 * Stream URL:       POST /playback/play/v1/tuneSource
 * Encryption key:   GET  /playback/key/v1/{keyId}
 */

#define EDGE_BASE "https://api.edge-gateway.siriusxm.com"
#define IMAGE_BASE "https://imgsrv-sxm-prod-device.streaming.siriusxm.com/"

/*
 * Build a full image URL from a relative path like "if/33/33dd...png"
 * Format: base64({"key":"<path>","edits":[{"resize":{"width":W,"height":H}}]})
 * Returns a newly allocated string.
 */
static char *
build_image_url (const char *relative_path,
                 int         width,
                 int         height)
{
  if (!relative_path || !*relative_path)
    return NULL;

  g_autofree char *json = g_strdup_printf (
    "{\"key\":\"%s\",\"edits\":[{\"resize\":{\"width\":%d,\"height\":%d}}]}",
    relative_path, width, height);

  g_autofree char *encoded = g_base64_encode ((const guchar *)json, strlen (json));

  return g_strconcat (IMAGE_BASE, encoded, NULL);
}

struct _SatwaveApiClient {
  GObject      parent_instance;
  SatwaveAuth *auth;
  GHashTable  *desc_cache;  /* slug → long description (static web content) */
};

G_DEFINE_TYPE (SatwaveApiClient, satwave_api_client, G_TYPE_OBJECT)

static void
satwave_api_client_dispose (GObject *object)
{
  SatwaveApiClient *self = SATWAVE_API_CLIENT (object);
  g_clear_object (&self->auth);
  g_clear_pointer (&self->desc_cache, g_hash_table_unref);
  G_OBJECT_CLASS (satwave_api_client_parent_class)->dispose (object);
}

static void
satwave_api_client_class_init (SatwaveApiClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_api_client_dispose;
}

static void
satwave_api_client_init (SatwaveApiClient *self)
{
  self->desc_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

SatwaveApiClient *
satwave_api_client_new (SatwaveAuth *auth)
{
  SatwaveApiClient *self = g_object_new (SATWAVE_TYPE_API_CLIENT, NULL);
  self->auth = g_object_ref (auth);
  return self;
}

/* Helper: create authenticated GET request */
static SoupMessage *
make_auth_get (SatwaveAuth *auth,
               const char  *url_path)
{
  g_autofree char *url = g_strdup_printf ("%s/%s", EDGE_BASE, url_path);
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);

  const char *token = satwave_auth_get_access_token (auth);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "Accept", "application/json; charset=utf-8");
  soup_message_headers_replace (headers, "x-sxm-tenant", "sxm");
  soup_message_headers_replace (headers, "x-sxm-platform", "browser");
  soup_message_headers_replace (headers, "Origin", "https://www.siriusxm.com");
  soup_message_headers_replace (headers, "Referer", "https://www.siriusxm.com/");

  if (token) {
    g_autofree char *bearer = g_strdup_printf ("Bearer %s", token);
    soup_message_headers_replace (headers, "Authorization", bearer);
  }

  return msg;
}

/* Helper: create authenticated POST request with JSON body */
static SoupMessage *
make_auth_post (SatwaveAuth *auth,
                const char  *url_path,
                const char  *body)
{
  g_autofree char *url = g_strdup_printf ("%s/%s", EDGE_BASE, url_path);
  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);

  const char *token = satwave_auth_get_access_token (auth);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);

  if (token) {
    g_autofree char *bearer = g_strdup_printf ("Bearer %s", token);
    soup_message_headers_replace (headers, "Authorization", bearer);
  }

  soup_message_headers_replace (headers, "Content-Type", "application/json; charset=UTF-8");
  soup_message_headers_replace (headers, "Accept", "application/json; charset=utf-8");
  soup_message_headers_replace (headers, "x-sxm-tenant", "sxm");
  soup_message_headers_replace (headers, "x-sxm-platform", "browser");
  soup_message_headers_replace (headers, "Origin", "https://www.siriusxm.com");
  soup_message_headers_replace (headers, "Referer", "https://www.siriusxm.com/");

  if (body) {
    GBytes *bytes = g_bytes_new (body, strlen (body));
    soup_message_set_request_body_from_bytes (msg, "application/json; charset=UTF-8", bytes);
    g_bytes_unref (bytes);
  }

  return msg;
}

/* ── Channel listing ── */

static void
on_channels_response (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, &error)) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  JsonNode *root_node = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root_node)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Channel response is not a JSON object");
    return;
  }

  JsonObject *root = json_node_get_object (root_node);

  /*
   * Edge Gateway response format:
   * { "container": { "sets": [ { "items": [...] } ] } }
   */
  JsonArray *items = NULL;

  if (json_object_has_member (root, "container")) {
    JsonObject *container = json_object_get_object_member (root, "container");
    if (json_object_has_member (container, "sets")) {
      JsonArray *sets = json_object_get_array_member (container, "sets");
      if (json_array_get_length (sets) > 0) {
        JsonObject *set0 = json_array_get_object_element (sets, 0);
        if (json_object_has_member (set0, "items"))
          items = json_object_get_array_member (set0, "items");
      }
    }
  }

  if (!items) {
    /* Log first 500 chars of response for debugging */
    g_autofree char *preview = g_strndup (data, MIN (size, 500));
    g_debug ("Channel response preview: %s", preview);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not find channel list in API response");
    return;
  }

  GListStore *store = g_list_store_new (SATWAVE_TYPE_CHANNEL);
  guint n_items = json_array_get_length (items);
  for (guint i = 0; i < n_items; i++) {
    JsonObject *item = json_array_get_object_element (items, i);

    /* Edge Gateway format */
    const char *id = "";
    const char *name = "Unknown";
    const char *desc = "";
    const char *category = "Uncategorized";
    const char *image_url = NULL;
    const char *entity_type = "channel-linear";
    int number = 0;

    if (json_object_has_member (item, "entity")) {
      /* Edge Gateway format: { entity: { type, id, texts, images }, decorations: {...} } */
      JsonObject *entity = json_object_get_object_member (item, "entity");
      id = json_object_get_string_member_with_default (entity, "id", "");
      entity_type = json_object_get_string_member_with_default (entity, "type", "channel-linear");

      if (json_object_has_member (entity, "texts")) {
        JsonObject *texts = json_object_get_object_member (entity, "texts");
        if (json_object_has_member (texts, "title")) {
          JsonObject *title_obj = json_object_get_object_member (texts, "title");
          name = json_object_get_string_member_with_default (title_obj, "default", "Unknown");
        }
        if (json_object_has_member (texts, "description")) {
          JsonObject *desc_obj = json_object_get_object_member (texts, "description");
          desc = json_object_get_string_member_with_default (desc_obj, "default", "");
        }
      }

      if (json_object_has_member (entity, "images")) {
        JsonObject *images = json_object_get_object_member (entity, "images");
        /* Try tile > logo for image */
        if (json_object_has_member (images, "tile")) {
          JsonObject *tile = json_object_get_object_member (images, "tile");
          if (json_object_has_member (tile, "aspect_1x1")) {
            JsonObject *a = json_object_get_object_member (tile, "aspect_1x1");
            if (json_object_has_member (a, "default")) {
              JsonObject *d = json_object_get_object_member (a, "default");
              image_url = json_object_get_string_member_with_default (d, "url", NULL);
            }
          }
        }
        if (!image_url && json_object_has_member (images, "logo")) {
          JsonObject *logo = json_object_get_object_member (images, "logo");
          if (json_object_has_member (logo, "aspect_5x4")) {
            JsonObject *a = json_object_get_object_member (logo, "aspect_5x4");
            if (json_object_has_member (a, "default")) {
              JsonObject *d = json_object_get_object_member (a, "default");
              image_url = json_object_get_string_member_with_default (d, "url", NULL);
            }
          }
        }
      }

      if (json_object_has_member (item, "decorations")) {
        JsonObject *deco = json_object_get_object_member (item, "decorations");
        number = (int) json_object_get_int_member_with_default (deco, "channelNumber",
          json_object_get_int_member_with_default (deco, "channelNumberCanonical", 0));
        /* Use genre (Pop, Rock, etc.) not contentTypeLabel (which is just "CHANNEL") */
        category = json_object_get_string_member_with_default (deco, "genre", "Uncategorized");

        /* Skip unentitled or off-air channels */
        if (json_object_get_boolean_member_with_default (deco, "unentitled", FALSE))
          continue;
      }
    } else {
      /* Legacy format: { channelId, name, siriusChannelNumber, ... } */
      id = json_object_get_string_member_with_default (item, "channelId", "");
      name = json_object_get_string_member_with_default (item, "name", "Unknown");
      number = (int) json_object_get_int_member_with_default (item, "siriusChannelNumber", 0);
      desc = json_object_get_string_member_with_default (item, "mediumDescription", "");

      if (json_object_has_member (item, "categories")) {
        JsonObject *cats = json_object_get_object_member (item, "categories");
        if (json_object_has_member (cats, "categories")) {
          JsonArray *cat_arr = json_object_get_array_member (cats, "categories");
          if (json_array_get_length (cat_arr) > 0) {
            JsonObject *cat = json_array_get_object_element (cat_arr, 0);
            category = json_object_get_string_member_with_default (cat, "name", "Uncategorized");
          }
        }
      }

      if (json_object_has_member (item, "images")) {
        JsonObject *images = json_object_get_object_member (item, "images");
        if (json_object_has_member (images, "images")) {
          JsonArray *img_arr = json_object_get_array_member (images, "images");
          if (json_array_get_length (img_arr) > 0) {
            JsonObject *img = json_array_get_object_element (img_arr, 0);
            image_url = json_object_get_string_member_with_default (img, "url", NULL);
          }
        }
      }
    }

    /* Use channelGuid if available */
    const char *guid = json_object_get_string_member_with_default (item, "channelGuid", id);

    /* Convert relative image path to full CDN URL (120x120 for grid cards) */
    g_autofree char *full_image_url = build_image_url (image_url, 240, 240);

    g_autoptr (SatwaveChannel) channel = satwave_channel_new (
      id, guid, name, number, desc, category, full_image_url, entity_type, FALSE);

    g_list_store_append (store, channel);
  }

  g_debug ("Parsed %u channels from Edge Gateway API",
           g_list_model_get_n_items (G_LIST_MODEL (store)));

  g_task_return_pointer (task, store, g_object_unref);
}

void
satwave_api_client_get_channels_async (SatwaveApiClient    *self,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  SoupMessage *msg = make_auth_get (self->auth,
    "relationship/v1/container/all-channels"
    "?containerId=3JoBfOCIwo6FmTpzM1S2H7"
    "&entityType=curated-grouping"
    "&entityId=403ab6a5-d3c9-4c2a-a722-a94a6a5fd056"
    "&offset=0"
    "&size=1000"
    "&useCuratedContext=false"
    "&setStyle=small_list");
  SoupSession *session = satwave_auth_get_session (self->auth);

  soup_session_send_and_read_async (session, msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    on_channels_response,
                                    g_object_ref (task));
  g_object_unref (msg);
}

GListStore *
satwave_api_client_get_channels_finish (SatwaveApiClient  *self,
                                        GAsyncResult      *result,
                                        GError           **error)
{
  (void)self;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Stream URL (tuneSource) ── */

static void
on_stream_url_response (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, &error)) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  JsonNode *root_node = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root_node)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "tuneSource response is not a JSON object");
    return;
  }
  JsonObject *root = json_node_get_object (root_node);

  /* Response: { id, type, streams: [{ urls: [{ url, isPrimary, name }] }] } */
  if (!json_object_has_member (root, "streams")) {
    /* Log full error for debugging */
    const char *desc = json_object_get_string_member_with_default (root, "description", "");
    const char *code = json_object_get_string_member_with_default (root, "code", "");
    g_autofree char *preview = g_strndup (data, MIN (size, 500));
    g_warning ("tuneSource error: desc='%s' code='%s' body=%s", desc, code, preview);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "tuneSource failed: %s (%s)", desc, code);
    return;
  }

  JsonArray *streams = json_object_get_array_member (root, "streams");
  if (json_array_get_length (streams) == 0) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Empty streams array");
    return;
  }

  /* Collect primary URL from each stream (xtra channels return multiple streams = tracks) */
  guint n_streams = json_array_get_length (streams);
  GPtrArray *url_array = g_ptr_array_new ();

  for (guint s = 0; s < n_streams; s++) {
    JsonObject *stream = json_array_get_object_element (streams, s);
    JsonArray *urls = json_object_get_array_member (stream, "urls");

    const char *best_url = NULL;
    for (guint u = 0; u < json_array_get_length (urls); u++) {
      JsonObject *url_obj = json_array_get_object_element (urls, u);
      gboolean is_primary = json_object_get_boolean_member_with_default (url_obj, "isPrimary", FALSE);
      const char *url = json_object_get_string_member_with_default (url_obj, "url", NULL);

      if (is_primary && url) { best_url = url; break; }
      if (!best_url && url) best_url = url;
    }

    if (best_url)
      g_ptr_array_add (url_array, g_strdup (best_url));

    /* Log track names for debugging */
    if (json_object_has_member (stream, "metadata")) {
      JsonObject *meta = json_object_get_object_member (stream, "metadata");
      if (json_object_has_member (meta, "xtra")) {
        JsonObject *xtra = json_object_get_object_member (meta, "xtra");
        const char *ch_name = json_object_get_string_member_with_default (xtra, "channelName", "");
        if (json_object_has_member (xtra, "items")) {
          JsonArray *xtra_items = json_object_get_array_member (xtra, "items");
          if (json_array_get_length (xtra_items) > 0) {
            JsonObject *item = json_array_get_object_element (xtra_items, 0);
            const char *title = json_object_get_string_member_with_default (item, "name", "?");
            const char *artist = json_object_get_string_member_with_default (item, "artistName", "?");
            g_debug ("  track[%u]: \"%s\" by %s [%s]", s, title, artist, ch_name);
          }
        }
      }
    }
  }

  if (url_array->len == 0) {
    g_ptr_array_unref (url_array);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "No stream URL found in response");
    return;
  }

  SatwaveStreamInfo *info = g_new0 (SatwaveStreamInfo, 1);
  info->n_urls = url_array->len;
  g_ptr_array_add (url_array, NULL); /* NULL-terminate */
  info->urls = (char **) g_ptr_array_free (url_array, FALSE);
  info->sequence_token = g_strdup (
    json_object_get_string_member_with_default (root, "sequenceToken", NULL));

  /* Extract sourceContextId from first stream's xtra metadata */
  JsonObject *first_stream = json_array_get_object_element (streams, 0);
  if (json_object_has_member (first_stream, "metadata")) {
    JsonObject *meta = json_object_get_object_member (first_stream, "metadata");
    if (json_object_has_member (meta, "xtra")) {
      JsonObject *xtra = json_object_get_object_member (meta, "xtra");
      info->source_context_id = g_strdup (
        json_object_get_string_member_with_default (xtra, "sourceContextId", NULL));
    }
  }

  g_task_return_pointer (task, info, (GDestroyNotify) satwave_stream_info_free);
}

void
satwave_stream_info_free (SatwaveStreamInfo *info)
{
  if (!info) return;
  g_strfreev (info->urls);
  g_free (info->sequence_token);
  g_free (info->source_context_id);
  g_free (info);
}

void
satwave_api_client_get_stream_url_async (SatwaveApiClient    *self,
                                         SatwaveChannel      *channel,
                                         const char          *sequence_token,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  const char *channel_id = satwave_channel_get_id (channel);
  const char *channel_type = satwave_channel_get_entity_type (channel);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
    json_builder_set_member_name (b, "id");
    json_builder_add_string_value (b, channel_id);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, channel_type);
    if (sequence_token && *sequence_token) {
      json_builder_set_member_name (b, "sequenceToken");
      json_builder_add_string_value (b, sequence_token);
    }
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) gen_root = json_builder_get_root (b);
  json_generator_set_root (gen, gen_root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  SoupMessage *msg = make_auth_post (self->auth, "playback/play/v1/tuneSource", body);
  SoupSession *session = satwave_auth_get_session (self->auth);

  soup_session_send_and_read_async (session, msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    on_stream_url_response,
                                    g_object_ref (task));
  g_object_unref (msg);
}

void
satwave_api_client_peek_async (SatwaveApiClient    *self,
                               SatwaveChannel      *channel,
                               const char          *sequence_token,
                               const char          *source_context_id,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  const char *channel_id = satwave_channel_get_id (channel);
  const char *channel_type = satwave_channel_get_entity_type (channel);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
    json_builder_set_member_name (b, "id");
    json_builder_add_string_value (b, channel_id);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, channel_type);
    json_builder_set_member_name (b, "hlsVersion");
    json_builder_add_string_value (b, "V3");
    json_builder_set_member_name (b, "mtcVersion");
    json_builder_add_string_value (b, "V2");
    if (sequence_token) {
      json_builder_set_member_name (b, "sequenceToken");
      json_builder_add_string_value (b, sequence_token);
    }
    if (source_context_id) {
      json_builder_set_member_name (b, "sourceContextId");
      json_builder_add_string_value (b, source_context_id);
    }
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) gen_root = json_builder_get_root (b);
  json_generator_set_root (gen, gen_root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  SoupMessage *msg = make_auth_post (self->auth, "playback/play/v1/peek", body);
  SoupSession *session = satwave_auth_get_session (self->auth);

  soup_session_send_and_read_async (session, msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    on_stream_url_response,
                                    g_object_ref (task));
  g_object_unref (msg);
}

SatwaveStreamInfo *
satwave_api_client_peek_finish (SatwaveApiClient  *self,
                                GAsyncResult      *result,
                                GError           **error)
{
  (void)self;
  return g_task_propagate_pointer (G_TASK (result), error);
}

SatwaveStreamInfo *
satwave_api_client_get_stream_url_finish (SatwaveApiClient  *self,
                                          GAsyncResult      *result,
                                          GError           **error)
{
  (void)self;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Now-playing metadata ── */

void
satwave_now_playing_free (SatwaveNowPlaying *np)
{
  if (!np) return;
  g_free (np->song_title);
  g_free (np->artist_name);
  g_free (np->show_name);
  g_free (np);
}

static void
on_now_playing_response (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, &error)) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  JsonNode *root_node = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root_node)) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "liveUpdate response is not a JSON object");
    return;
  }
  JsonObject *root = json_node_get_object (root_node);

  SatwaveNowPlaying *np = g_new0 (SatwaveNowPlaying, 1);

  /* Extract from items[0] — the currently playing track */
  if (json_object_has_member (root, "items")) {
    JsonArray *items = json_object_get_array_member (root, "items");
    if (json_array_get_length (items) > 0) {
      JsonObject *item = json_array_get_object_element (items, 0);
      np->song_title = g_strdup (
        json_object_get_string_member_with_default (item, "name", NULL));
      np->artist_name = g_strdup (
        json_object_get_string_member_with_default (item, "artistName", NULL));
    }
  }

  /* Extract show name from episodes[0] */
  if (json_object_has_member (root, "episodes")) {
    JsonArray *episodes = json_object_get_array_member (root, "episodes");
    if (json_array_get_length (episodes) > 0) {
      JsonObject *ep = json_array_get_object_element (episodes, 0);
      np->show_name = g_strdup (
        json_object_get_string_member_with_default (ep, "showName", NULL));
    }
  }

  g_task_return_pointer (task, np, (GDestroyNotify) satwave_now_playing_free);
}

void
satwave_api_client_get_now_playing_async (SatwaveApiClient    *self,
                                          const char          *channel_id,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  /* Build timestamps: 5 minutes ago to now */
  g_autoptr (GDateTime) now = g_date_time_new_now_utc ();
  g_autoptr (GDateTime) past = g_date_time_add_minutes (now, -5);
  g_autofree char *now_str = g_date_time_format_iso8601 (now);
  g_autofree char *past_str = g_date_time_format_iso8601 (past);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
    json_builder_set_member_name (b, "channelId");
    json_builder_add_string_value (b, channel_id);
    json_builder_set_member_name (b, "startTimestamp");
    json_builder_add_string_value (b, past_str);
    json_builder_set_member_name (b, "endTimestamp");
    json_builder_add_string_value (b, now_str);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) gen_root = json_builder_get_root (b);
  json_generator_set_root (gen, gen_root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  SoupMessage *msg = make_auth_post (self->auth, "playback/play/v1/liveUpdate", body);
  SoupSession *session = satwave_auth_get_session (self->auth);

  soup_session_send_and_read_async (session, msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    on_now_playing_response,
                                    g_object_ref (task));
  g_object_unref (msg);
}

SatwaveNowPlaying *
satwave_api_client_get_now_playing_finish (SatwaveApiClient  *self,
                                           GAsyncResult      *result,
                                           GError           **error)
{
  (void)self;
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ── Channel description from siriusxm.com ── */

/* Slugify: "SiriusXM Hits 1" -> "siriusxm-hits-1" */
static char *
slugify (const char *name)
{
  GString *slug = g_string_new (NULL);
  for (const char *p = name; *p; p++) {
    if (g_ascii_isalnum (*p))
      g_string_append_c (slug, g_ascii_tolower (*p));
    else if (*p == ' ' || *p == '-')
      g_string_append_c (slug, '-');
  }
  /* Collapse multiple dashes */
  g_autofree char *raw = g_string_free (slug, FALSE);
  GString *clean = g_string_new (NULL);
  gboolean last_was_dash = FALSE;
  for (const char *p = raw; *p; p++) {
    if (*p == '-') {
      if (!last_was_dash)
        g_string_append_c (clean, '-');
      last_was_dash = TRUE;
    } else {
      g_string_append_c (clean, *p);
      last_was_dash = FALSE;
    }
  }
  return g_string_free (clean, FALSE);
}

static void
on_channel_desc_response (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  gsize size;
  const char *html = g_bytes_get_data (bytes, &size);
  g_autofree char *desc = NULL;

  /* Extract og:description from the HTML. The buffer is NOT NUL-terminated,
   * so every search must be length-bounded. */
  const char *og_start = g_strstr_len (html, size, "og:description");
  if (!og_start)
    og_start = g_strstr_len (html, size, "name=\"description\"");

  if (og_start) {
    const char *content = g_strstr_len (og_start, size - (og_start - html), "content=\"");
    if (content) {
      content += 9; /* skip content=" */
      const char *end = memchr (content, '"', size - (content - html));
      if (end && end > content)
        desc = g_strndup (content, end - content);
    }
  }

  if (desc) {
    /* Basic HTML entity replacement */
    GString *result_str = g_string_new (desc);
    g_string_replace (result_str, "&amp;", "&", 0);
    g_string_replace (result_str, "&#x27;", "'", 0);
    g_string_replace (result_str, "&quot;", "\"", 0);
    g_string_replace (result_str, "&#39;", "'", 0);
    g_string_replace (result_str, "&lt;", "<", 0);
    g_string_replace (result_str, "&gt;", ">", 0);

    /* Cache by slug — the page content is static */
    SatwaveApiClient *self = g_task_get_source_object (task);
    const char *slug = g_object_get_data (G_OBJECT (task), "slug");
    if (self && slug)
      g_hash_table_replace (self->desc_cache,
                            g_strdup (slug), g_strdup (result_str->str));

    g_task_return_pointer (task, g_string_free (result_str, FALSE), g_free);
    return;
  }

  g_task_return_pointer (task, g_strdup (""), g_free);
}

void
satwave_api_client_get_channel_desc_async (SatwaveApiClient    *self,
                                           const char          *channel_name,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  g_autofree char *slug = slugify (channel_name);

  /* Serve repeat opens from the cache — the page is several hundred KB */
  const char *cached = g_hash_table_lookup (self->desc_cache, slug);
  if (cached) {
    g_task_return_pointer (task, g_strdup (cached), g_free);
    return;
  }

  g_object_set_data_full (G_OBJECT (task), "slug", g_strdup (slug), g_free);

  g_autofree char *url = g_strdup_printf ("https://www.siriusxm.com/channels/%s", slug);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "User-Agent",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36");
  soup_message_headers_replace (headers, "Accept", "text/html");

  SoupSession *session = satwave_auth_get_session (self->auth);
  soup_session_send_and_read_async (session, msg,
                                    G_PRIORITY_LOW,
                                    cancellable,
                                    on_channel_desc_response,
                                    g_object_ref (task));
  g_object_unref (msg);
}

char *
satwave_api_client_get_channel_desc_finish (SatwaveApiClient  *self,
                                            GAsyncResult      *result,
                                            GError           **error)
{
  (void)self;
  return g_task_propagate_pointer (G_TASK (result), error);
}
