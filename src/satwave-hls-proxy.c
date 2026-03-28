#include "satwave-hls-proxy.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/*
 * Internal HLS proxy server.
 *
 * Sits between GStreamer and the SiriusXM/Akamai CDN:
 *   1. Rewrites HLS playlist URLs to point to localhost
 *   2. Serves the known AES-128 decryption key at /key/1
 *   3. Injects Bearer auth on upstream requests
 *   4. Proxies segment fetches to CDN
 *
 * The Edge Gateway API returns stream URLs that may include auth in the URL
 * itself or require Bearer token. This proxy handles both cases.
 */

/* Known AES-128 decryption key: base64 "0Nsco7MAgxowGvkUT8aYag==" decoded */
static const char SXM_AES_KEY_RAW[16] = {
  0xd0, 0xdb, 0x1c, 0xa3, 0xb3, 0x00, 0x83, 0x1a,
  0x30, 0x1a, 0xf9, 0x14, 0x4f, 0xc6, 0x98, 0x6a
};

struct _SatwaveHlsProxy {
  GObject      parent_instance;

  SatwaveAuth *auth;
  SoupServer  *server;
  SoupSession *fetch_session;  /* Separate session for upstream fetches */
  guint16      port;
  char        *upstream_url;   /* Full upstream HLS URL from tuneSource */
  char        *upstream_base;  /* Base URL for resolving relative segment paths */
};

G_DEFINE_TYPE (SatwaveHlsProxy, satwave_hls_proxy, G_TYPE_OBJECT)

/* Extract base URL (everything up to last /) from a full URL */
static char *
url_base (const char *url)
{
  const char *last_slash = strrchr (url, '/');
  if (!last_slash || last_slash == url)
    return g_strdup (url);
  return g_strndup (url, last_slash - url + 1);
}

/* Fetch content from upstream, injecting auth if needed */
static GBytes *
fetch_upstream (SatwaveHlsProxy *self,
                const char      *url)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  if (!msg) {
    g_warning ("Invalid upstream URL: %s", url);
    return NULL;
  }

  /* Add Bearer auth if URL is on SiriusXM domain */
  const char *token = satwave_auth_get_access_token (self->auth);
  if (token && (strstr (url, "siriusxm") || strstr (url, "edge-gateway"))) {
    SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
    g_autofree char *bearer = g_strdup_printf ("Bearer %s", token);
    soup_message_headers_replace (headers, "Authorization", bearer);
  }

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read (self->fetch_session, msg, NULL, &error);
  guint status = soup_message_get_status (msg);
  g_object_unref (msg);

  if (error) {
    g_warning ("Upstream fetch failed for %s: %s", url, error->message);
    g_error_free (error);
    return NULL;
  }

  if (status >= 400) {
    g_warning ("Upstream returned HTTP %u for %s", status, url);
    g_bytes_unref (bytes);
    return NULL;
  }

  return bytes;
}

/* Rewrite HLS playlist: replace segment/key URLs to go through our proxy.
 * base_url is the directory URL of the playlist being rewritten (for relative resolution). */
static char *
rewrite_playlist (SatwaveHlsProxy *self,
                  const char      *content,
                  const char      *base_url)
{
  GString *result = g_string_new (NULL);
  g_auto (GStrv) lines = g_strsplit (content, "\n", -1);

  for (int i = 0; lines[i]; i++) {
    /* Strip trailing \r from \r\n line endings */
    g_strchomp (lines[i]);
    const char *line = lines[i];

    if (line[0] == '\0') {
      g_string_append_c (result, '\n');
      continue;
    }

    if (g_str_has_prefix (line, "#EXT-X-KEY:")) {
      /* Extract key ID from URI and proxy through our /key/ handler.
       * URI format: "https://api.edge-gateway.siriusxm.com/playback/key/v1/{keyId}" */
      const char *uri_start = strstr (line, "URI=\"");
      const char *key_id = "00000000-0000-0000-0000-000000000000";
      if (uri_start) {
        const char *key_path = strstr (uri_start, "/key/v1/");
        if (key_path) {
          key_path += 8; /* skip "/key/v1/" */
          const char *key_end = strchr (key_path, '"');
          if (key_end) {
            g_autofree char *extracted = g_strndup (key_path, key_end - key_path);
            /* Use extracted key ID in our proxy URL */
            g_string_append_printf (result,
              "#EXT-X-KEY:METHOD=AES-128,URI=\"http://127.0.0.1:%u/key/%s\"\n",
              self->port, extracted);
            goto next_line;
          }
        }
      }
      /* Fallback: default key */
      g_string_append_printf (result,
        "#EXT-X-KEY:METHOD=AES-128,URI=\"http://127.0.0.1:%u/key/%s\"\n",
        self->port, key_id);
      next_line: (void)0;
    } else if (line[0] != '#') {
      /* This is a URI line (segment or sub-playlist) */
      if (g_str_has_prefix (line, "http://") || g_str_has_prefix (line, "https://")) {
        /* Absolute URL — URL-encode and pass through proxy */
        g_autofree char *encoded = g_uri_escape_string (line, NULL, FALSE);
        g_string_append_printf (result,
          "http://127.0.0.1:%u/proxy?url=%s\n",
          self->port, encoded);
      } else {
        /* Relative URL — resolve against the base URL of THIS playlist */
        g_autofree char *full = g_strconcat (base_url, line, NULL);
        g_autofree char *encoded = g_uri_escape_string (full, NULL, FALSE);
        g_string_append_printf (result,
          "http://127.0.0.1:%u/proxy?url=%s\n",
          self->port, encoded);
      }
    } else {
      /* Pass through other HLS tags unchanged */
      g_string_append (result, line);
      g_string_append_c (result, '\n');
    }
  }

  return g_string_free (result, FALSE);
}

/* Handler: /key/{keyId} — fetch and serve AES decryption key from SXM API */
static void
handle_key_request (SoupServer        *server,
                    SoupServerMessage *msg,
                    const char        *path,
                    GHashTable        *query,
                    gpointer           user_data)
{
  (void)server; (void)query;
  SatwaveHlsProxy *self = SATWAVE_HLS_PROXY (user_data);

  /* Extract key ID from path: /key/{keyId} */
  const char *key_id = path + 5; /* skip "/key/" */
  if (!key_id || !*key_id)
    key_id = "00000000-0000-0000-0000-000000000000";

  /* Check if it's the well-known default key */
  if (g_strcmp0 (key_id, "1") == 0 ||
      g_strcmp0 (key_id, "00000000-0000-0000-0000-000000000000") == 0) {
    SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
    soup_message_headers_replace (resp_headers, "Content-Type", "application/octet-stream");
    soup_message_headers_replace (resp_headers, "Access-Control-Allow-Origin", "*");
    soup_server_message_set_response (msg, "application/octet-stream",
                                      SOUP_MEMORY_COPY,
                                      SXM_AES_KEY_RAW,
                                      sizeof (SXM_AES_KEY_RAW));
    soup_server_message_set_status (msg, 200, NULL);
    return;
  }

  /* Fetch the key from the edge-gateway API */
  g_autofree char *key_url = g_strdup_printf (
    "https://api.edge-gateway.siriusxm.com/playback/key/v1/%s", key_id);

  SoupMessage *key_msg = soup_message_new (SOUP_METHOD_GET, key_url);
  SoupMessageHeaders *req_headers = soup_message_get_request_headers (key_msg);
  soup_message_headers_replace (req_headers, "Accept", "application/json");
  soup_message_headers_replace (req_headers, "x-sxm-tenant", "sxm");
  soup_message_headers_replace (req_headers, "x-sxm-platform", "browser");

  const char *token = satwave_auth_get_access_token (self->auth);
  if (token) {
    g_autofree char *bearer = g_strdup_printf ("Bearer %s", token);
    soup_message_headers_replace (req_headers, "Authorization", bearer);
  }

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read (self->fetch_session, key_msg, NULL, &error);
  g_object_unref (key_msg);

  if (error || !bytes) {
    g_warning ("Failed to fetch key %s: %s", key_id, error ? error->message : "no data");
    g_clear_error (&error);
    /* Fallback to default key */
    SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
    soup_message_headers_replace (resp_headers, "Content-Type", "application/octet-stream");
    soup_server_message_set_response (msg, "application/octet-stream",
                                      SOUP_MEMORY_COPY,
                                      SXM_AES_KEY_RAW,
                                      sizeof (SXM_AES_KEY_RAW));
    soup_server_message_set_status (msg, 200, NULL);
    return;
  }

  /* Parse JSON response: {"keyId":"...","key":"base64_key"} */
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (json_parser_load_from_data (parser, data, size, NULL)) {
    JsonObject *obj = json_node_get_object (json_parser_get_root (parser));
    const char *b64_key = json_object_get_string_member_with_default (obj, "key", NULL);

    if (b64_key) {
      gsize key_len = 0;
      g_autofree guchar *raw_key = g_base64_decode (b64_key, &key_len);

      if (raw_key && key_len == 16) {
        SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
        soup_message_headers_replace (resp_headers, "Content-Type", "application/octet-stream");
        soup_message_headers_replace (resp_headers, "Access-Control-Allow-Origin", "*");
        soup_server_message_set_response (msg, "application/octet-stream",
                                          SOUP_MEMORY_COPY,
                                          (const char *)raw_key, key_len);
        soup_server_message_set_status (msg, 200, NULL);
        g_bytes_unref (bytes);
        return;
      }
    }
  }

  g_bytes_unref (bytes);

  /* Last resort fallback */
  g_warning ("Could not parse key response for %s, using default", key_id);
  SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_replace (resp_headers, "Content-Type", "application/octet-stream");
  soup_server_message_set_response (msg, "application/octet-stream",
                                    SOUP_MEMORY_COPY,
                                    SXM_AES_KEY_RAW,
                                    sizeof (SXM_AES_KEY_RAW));
  soup_server_message_set_status (msg, 200, NULL);
}

/* Handler: /master — serve the top-level playlist (entry point for GStreamer) */
static void
handle_master_request (SoupServer        *server,
                       SoupServerMessage *msg,
                       const char        *path,
                       GHashTable        *query,
                       gpointer           user_data)
{
  (void)server; (void)path; (void)query;
  SatwaveHlsProxy *self = SATWAVE_HLS_PROXY (user_data);

  if (!self->upstream_url) {
    soup_server_message_set_status (msg, 503, "No upstream URL set");
    return;
  }

  g_autoptr (GBytes) bytes = fetch_upstream (self, self->upstream_url);
  if (!bytes) {
    soup_server_message_set_status (msg, 502, "Upstream fetch failed");
    return;
  }

  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  /* Check if this is a playlist (text) or binary data */
  if (size > 0 && data[0] == '#') {
    g_autofree char *rewritten = rewrite_playlist (self, data, self->upstream_base);

    SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
    soup_message_headers_replace (resp_headers, "Access-Control-Allow-Origin", "*");

    soup_server_message_set_response (msg, "application/vnd.apple.mpegurl",
                                      SOUP_MEMORY_COPY,
                                      rewritten, strlen (rewritten));
  } else {
    SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
    soup_message_headers_replace (resp_headers, "Access-Control-Allow-Origin", "*");
    soup_server_message_set_response (msg, "application/octet-stream",
                                      SOUP_MEMORY_COPY, data, size);
  }

  soup_server_message_set_status (msg, 200, NULL);
}

/* Handler: /proxy?url=<encoded_url> — generic proxy for segments and sub-playlists */
static void
handle_proxy_request (SoupServer        *server,
                      SoupServerMessage *msg,
                      const char        *path,
                      GHashTable        *query,
                      gpointer           user_data)
{
  (void)server; (void)path;
  SatwaveHlsProxy *self = SATWAVE_HLS_PROXY (user_data);

  const char *encoded_url = query ? g_hash_table_lookup (query, "url") : NULL;
  if (!encoded_url) {
    soup_server_message_set_status (msg, 400, "Missing url parameter");
    return;
  }

  g_autofree char *upstream = g_uri_unescape_string (encoded_url, NULL);
  if (!upstream) {
    soup_server_message_set_status (msg, 400, "Invalid url parameter");
    return;
  }

  g_autoptr (GBytes) bytes = fetch_upstream (self, upstream);
  if (!bytes) {
    soup_server_message_set_status (msg, 502, "Upstream fetch failed");
    return;
  }

  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  SoupMessageHeaders *resp_headers = soup_server_message_get_response_headers (msg);
  soup_message_headers_replace (resp_headers, "Access-Control-Allow-Origin", "*");

  /* If it's a playlist, rewrite it; otherwise pass through */
  if (g_str_has_suffix (upstream, ".m3u8") || (size > 0 && data[0] == '#')) {
    /* Compute base URL from THIS playlist's URL for correct relative resolution */
    g_autofree char *this_base = url_base (upstream);

    g_autofree char *rewritten = rewrite_playlist (self, data, this_base);
    soup_server_message_set_response (msg, "application/vnd.apple.mpegurl",
                                      SOUP_MEMORY_COPY,
                                      rewritten, strlen (rewritten));
  } else {
    const char *ct = "application/octet-stream";
    if (g_str_has_suffix (upstream, ".aac"))
      ct = "audio/aac";
    else if (g_str_has_suffix (upstream, ".ts"))
      ct = "video/mp2t";
    soup_server_message_set_response (msg, ct, SOUP_MEMORY_COPY, data, size);
  }

  soup_server_message_set_status (msg, 200, NULL);
}

static void
satwave_hls_proxy_dispose (GObject *object)
{
  SatwaveHlsProxy *self = SATWAVE_HLS_PROXY (object);

  satwave_hls_proxy_stop (self);
  g_clear_object (&self->auth);
  g_clear_object (&self->fetch_session);
  g_clear_pointer (&self->upstream_url, g_free);
  g_clear_pointer (&self->upstream_base, g_free);

  G_OBJECT_CLASS (satwave_hls_proxy_parent_class)->dispose (object);
}

static void
satwave_hls_proxy_class_init (SatwaveHlsProxyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_hls_proxy_dispose;
}

static void
satwave_hls_proxy_init (SatwaveHlsProxy *self)
{
  self->server = soup_server_new (NULL, NULL);
  self->fetch_session = soup_session_new_with_options (
    "user-agent",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    NULL);
}

SatwaveHlsProxy *
satwave_hls_proxy_new (SatwaveAuth *auth)
{
  SatwaveHlsProxy *self = g_object_new (SATWAVE_TYPE_HLS_PROXY, NULL);
  self->auth = g_object_ref (auth);
  return self;
}

gboolean
satwave_hls_proxy_start (SatwaveHlsProxy  *self,
                         GError          **error)
{
  soup_server_add_handler (self->server, "/key",
                           handle_key_request, self, NULL);
  soup_server_add_handler (self->server, "/master",
                           handle_master_request, self, NULL);
  soup_server_add_handler (self->server, "/proxy",
                           handle_proxy_request, self, NULL);

  if (!soup_server_listen_local (self->server, 0,
                                 SOUP_SERVER_LISTEN_IPV4_ONLY, error))
    return FALSE;

  GSList *uris = soup_server_get_uris (self->server);
  if (uris) {
    GUri *uri = uris->data;
    self->port = g_uri_get_port (uri);
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  }

  g_debug ("HLS proxy listening on port %u", self->port);
  return TRUE;
}

void
satwave_hls_proxy_stop (SatwaveHlsProxy *self)
{
  if (self->server)
    soup_server_disconnect (self->server);
}

char *
satwave_hls_proxy_get_url (SatwaveHlsProxy *self,
                           const char      *channel_id)
{
  (void)channel_id;
  return g_strdup_printf ("http://127.0.0.1:%u/master", self->port);
}

guint16
satwave_hls_proxy_get_port (SatwaveHlsProxy *self)
{
  return self->port;
}

void
satwave_hls_proxy_set_upstream (SatwaveHlsProxy *self,
                                const char      *upstream_url)
{
  g_free (self->upstream_url);
  self->upstream_url = g_strdup (upstream_url);

  g_free (self->upstream_base);
  self->upstream_base = upstream_url ? url_base (upstream_url) : NULL;

  g_debug ("Upstream set: %s (base: %s)",
           self->upstream_url ? self->upstream_url : "none",
           self->upstream_base ? self->upstream_base : "none");
}
