#include "satwave-auth.h"
#include <json-glib/json-glib.h>
#include <libsecret/secret.h>
#include <string.h>

/*
 * SiriusXM Edge Gateway API authentication.
 *
 * Flow:
 *   1. POST device/v1/devices              → deviceId + device grant
 *   2. POST session/v1/sessions/anonymous   → anonymous access token
 *   3. POST identity/v1/identities/authenticate/password → identity grant
 *   4. POST session/v1/sessions/authenticated → JWT access token
 *
 * No 2FA/OTP required — password auth works without it.
 * Tokens are cached to avoid repeated logins.
 */

#define EDGE_BASE "https://api.edge-gateway.siriusxm.com"

static const SecretSchema satwave_schema = {
  "com.github.satwave.Credentials",
  SECRET_SCHEMA_NONE,
  {
    { "type", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  },
  0 /* reserved */
};

struct _SatwaveAuth {
  GObject      parent_instance;

  SoupSession *session;
  char        *username;
  gboolean     authenticated;

  /* Edge Gateway tokens */
  char        *device_id;
  char        *device_grant;
  char        *access_token;
  char        *identity_grant;
  char        *refresh_token;

  guint        refresh_source_id;
};

G_DEFINE_TYPE (SatwaveAuth, satwave_auth, G_TYPE_OBJECT)

/* Forward declarations */
static void step2_anonymous_session (GTask *task);
static void step3_password_auth     (GTask *task);
static void step4_auth_session      (GTask *task);
static void on_password_lookup      (GObject *source, GAsyncResult *result, gpointer user_data);
static void on_auto_login_done      (GObject *source, GAsyncResult *result, gpointer user_data);
static void store_credentials       (const char *username, const char *password);

/* Task data to carry credentials through the async chain */
typedef struct {
  char *username;
  char *password;
} LoginData;

static void
login_data_free (gpointer data)
{
  LoginData *ld = data;
  g_free (ld->username);
  g_free (ld->password);
  g_free (ld);
}

/* ── Helpers ── */

static SoupMessage *
make_json_post (const char *url_path,
                const char *bearer_token,
                const char *body)
{
  g_autofree char *url = g_strdup_printf ("%s/%s", EDGE_BASE, url_path);
  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);

  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "Content-Type", "application/json; charset=UTF-8");
  soup_message_headers_replace (headers, "Accept", "application/json");
  soup_message_headers_replace (headers, "x-sxm-tenant", "sxm");

  if (bearer_token) {
    g_autofree char *auth = g_strdup_printf ("Bearer %s", bearer_token);
    soup_message_headers_replace (headers, "Authorization", auth);
  }

  if (body) {
    GBytes *bytes = g_bytes_new (body, strlen (body));
    soup_message_set_request_body_from_bytes (msg, "application/json; charset=UTF-8", bytes);
    g_bytes_unref (bytes);
  }

  return msg;
}

static JsonObject *
parse_json_response (GBytes      *bytes,
                     GError     **error)
{
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, size, error))
    return NULL;

  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "API response is not a JSON object");
    return NULL;
  }

  return json_object_ref (json_node_get_object (root));
}

/* ── Step 1: Register device ── */

static void
on_register_device (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  /* Check HTTP status */
  SoupMessage *msg = g_object_get_data (G_OBJECT (task), "step1-msg");
  guint status_code = soup_message_get_status (msg);

  g_autoptr (JsonObject) obj = parse_json_response (bytes, &error);
  if (!obj) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (status_code < 200 || status_code >= 300) {
    const char *detail = json_object_get_string_member_with_default (obj, "message", NULL);
    if (!detail) {
      /* Dump full response for debugging */
      g_autoptr (JsonGenerator) dbg_gen = json_generator_new ();
      g_autoptr (JsonNode) dbg_node = json_node_init_object (json_node_alloc (), obj);
      json_generator_set_root (dbg_gen, dbg_node);
      g_autofree char *dbg = json_generator_to_data (dbg_gen, NULL);
      g_warning ("Device registration response: %s", dbg);
      detail = "See log for details";
    }
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Device registration failed (%u): %s", status_code, detail);
    return;
  }

  g_free (self->device_id);
  g_free (self->device_grant);
  self->device_id = g_strdup (json_object_get_string_member_with_default (obj, "deviceId", ""));
  self->device_grant = g_strdup (json_object_get_string_member_with_default (obj, "grant", ""));

  g_debug ("Device registered: %s", self->device_id);

  step2_anonymous_session (g_steal_pointer (&task));
}

/* ── Step 2: Anonymous session ── */

static void
on_anonymous_session (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_autoptr (JsonObject) obj = parse_json_response (bytes, &error);
  if (!obj) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_free (self->access_token);
  self->access_token = g_strdup (
    json_object_get_string_member_with_default (obj, "accessToken", ""));

  g_debug ("Anonymous session created");

  step3_password_auth (g_steal_pointer (&task));
}

static void
step2_anonymous_session (GTask *task)
{
  SatwaveAuth *self = g_task_get_source_object (task);

  SoupMessage *msg = make_json_post ("session/v1/sessions/anonymous",
                                     self->device_grant, NULL);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_anonymous_session, task);
  g_object_unref (msg);
}

/* ── Step 3: Password authentication ── */

static void
on_password_auth (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  SoupMessage *msg = g_object_get_data (G_OBJECT (task), "step3-msg");
  guint status_code = soup_message_get_status (msg);

  g_autoptr (JsonObject) obj = parse_json_response (bytes, &error);
  if (!obj) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (status_code == 401 || status_code == 403) {
    const char *detail = json_object_get_string_member_with_default (obj, "message", "");
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Authentication failed: %s", detail);
    return;
  }

  if (status_code < 200 || status_code >= 300) {
    const char *detail = json_object_get_string_member_with_default (obj, "message", "Unknown error");
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Password auth failed (%u): %s", status_code, detail);
    return;
  }

  g_free (self->identity_grant);
  self->identity_grant = g_strdup (
    json_object_get_string_member_with_default (obj, "grant", ""));

  g_debug ("Identity authenticated");

  step4_auth_session (g_steal_pointer (&task));
}

static void
step3_password_auth (GTask *task)
{
  SatwaveAuth *self = g_task_get_source_object (task);
  LoginData *ld = g_task_get_task_data (task);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
    json_builder_set_member_name (b, "handle");
    json_builder_add_string_value (b, ld->username);
    json_builder_set_member_name (b, "password");
    json_builder_add_string_value (b, ld->password);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) gen_root = json_builder_get_root (b);
  json_generator_set_root (gen, gen_root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  SoupMessage *msg = make_json_post ("identity/v1/identities/authenticate/password",
                                     self->access_token, body);

  g_object_set_data_full (G_OBJECT (task), "step3-msg",
                          g_object_ref (msg), g_object_unref);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_password_auth, task);
  g_object_unref (msg);
}

/* ── Step 4: Authenticated session ── */

static void
on_auth_session (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_autoptr (JsonObject) obj = parse_json_response (bytes, &error);
  if (!obj) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  /* Extract the final access token */
  g_free (self->access_token);
  self->access_token = g_strdup (
    json_object_get_string_member_with_default (obj, "accessToken", ""));

  /* Store refresh token if available */
  if (json_object_has_member (obj, "refreshToken")) {
    g_free (self->refresh_token);
    self->refresh_token = g_strdup (
      json_object_get_string_member (obj, "refreshToken"));
  }

  if (!self->access_token || self->access_token[0] == '\0') {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "No access token in authenticated session response");
    return;
  }

  self->authenticated = TRUE;
  g_debug ("Authenticated session established (token length: %lu)",
           (unsigned long) strlen (self->access_token));

  g_task_return_boolean (task, TRUE);
}

static void
step4_auth_session (GTask *task)
{
  SatwaveAuth *self = g_task_get_source_object (task);

  SoupMessage *msg = make_json_post ("session/v1/sessions/authenticated",
                                     self->identity_grant, NULL);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_auth_session, task);
  g_object_unref (msg);
}

/* ── GObject lifecycle ── */

static void
satwave_auth_dispose (GObject *object)
{
  SatwaveAuth *self = SATWAVE_AUTH (object);

  if (self->refresh_source_id > 0) {
    g_source_remove (self->refresh_source_id);
    self->refresh_source_id = 0;
  }

  g_clear_object (&self->session);
  g_clear_pointer (&self->access_token, g_free);
  g_clear_pointer (&self->identity_grant, g_free);
  g_clear_pointer (&self->device_id, g_free);
  g_clear_pointer (&self->device_grant, g_free);
  g_clear_pointer (&self->refresh_token, g_free);
  g_clear_pointer (&self->username, g_free);

  G_OBJECT_CLASS (satwave_auth_parent_class)->dispose (object);
}

static void
satwave_auth_class_init (SatwaveAuthClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_auth_dispose;
}

static void
satwave_auth_init (SatwaveAuth *self)
{
  self->session = soup_session_new_with_options (
    "user-agent",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    NULL);
}

SatwaveAuth *
satwave_auth_new (void)
{
  return g_object_new (SATWAVE_TYPE_AUTH, NULL);
}

/* ── Public: Login ── */

void
satwave_auth_login_async (SatwaveAuth         *self,
                          const char          *username,
                          const char          *password,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  LoginData *ld = g_new0 (LoginData, 1);
  ld->username = g_strdup (username);
  ld->password = g_strdup (password);
  g_task_set_task_data (task, ld, login_data_free);

  /* Dup before free — the restore path passes self->username as the
   * argument, so freeing first reads freed memory */
  char *username_copy = g_strdup (username);
  g_free (self->username);
  self->username = username_copy;
  self->authenticated = FALSE;

  /* Step 1: Register device */
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
    json_builder_set_member_name (b, "devicePlatform");
    json_builder_add_string_value (b, "web-desktop");
    json_builder_set_member_name (b, "deviceAttributes");
    json_builder_begin_object (b);
      json_builder_set_member_name (b, "browser");
      json_builder_begin_object (b);
        json_builder_set_member_name (b, "browserVersion");
        json_builder_add_string_value (b, "120.0.0.0");
        json_builder_set_member_name (b, "browser");
        json_builder_add_string_value (b, "Chrome");
        json_builder_set_member_name (b, "userAgent");
        json_builder_add_string_value (b,
          "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
          "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        json_builder_set_member_name (b, "app");
        json_builder_add_string_value (b, "sxm_web_prod");
        json_builder_set_member_name (b, "appVersion");
        json_builder_add_string_value (b, "5.36.514");
      json_builder_end_object (b);
    json_builder_end_object (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) gen_root = json_builder_get_root (b);
  json_generator_set_root (gen, gen_root);
  g_autofree char *body = json_generator_to_data (gen, NULL);

  SoupMessage *msg = make_json_post ("device/v1/devices", NULL, body);

  /* Store msg ref so we can check status code in callback */
  g_object_set_data_full (G_OBJECT (task), "step1-msg",
                          g_object_ref (msg), g_object_unref);

  soup_session_send_and_read_async (self->session, msg,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    on_register_device,
                                    g_object_ref (task));
  g_object_unref (msg);
}

gboolean
satwave_auth_login_finish (SatwaveAuth   *self,
                           GAsyncResult  *result,
                           GError       **error)
{
  (void)self;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Public: Restore credentials from keyring ── */

static void
on_username_lookup (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autofree char *username = secret_password_lookup_finish (result, &error);
  if (error || !username) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No stored credentials found");
    return;
  }

  g_free (self->username);
  self->username = g_strdup (username);

  secret_password_lookup (&satwave_schema,
                          g_task_get_cancellable (task),
                          on_password_lookup,
                          g_object_ref (task),
                          "type", "password",
                          "account", username,
                          NULL);
}

static void
on_password_lookup (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  g_autofree char *password = secret_password_lookup_finish (result, &error);
  if (error || !password) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No stored password found");
    return;
  }

  satwave_auth_login_async (self, self->username, password,
                            g_task_get_cancellable (task),
                            on_auto_login_done,
                            g_object_ref (task));
}

static void
on_auto_login_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (GTask) task = G_TASK (user_data);
  SatwaveAuth *self = SATWAVE_AUTH (source);
  g_autoptr (GError) error = NULL;

  if (!satwave_auth_login_finish (self, result, &error)) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_task_return_boolean (task, TRUE);
}

void
satwave_auth_restore_async (SatwaveAuth         *self,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);

  secret_password_lookup (&satwave_schema,
                          cancellable,
                          on_username_lookup,
                          g_object_ref (task),
                          "type", "username",
                          "account", "default",
                          NULL);
}

gboolean
satwave_auth_restore_finish (SatwaveAuth   *self,
                             GAsyncResult  *result,
                             GError       **error)
{
  (void)self;
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ── Public: Logout ── */

void
satwave_auth_logout (SatwaveAuth *self)
{
  self->authenticated = FALSE;
  g_clear_pointer (&self->access_token, g_free);
  g_clear_pointer (&self->identity_grant, g_free);
  g_clear_pointer (&self->device_id, g_free);
  g_clear_pointer (&self->device_grant, g_free);
  g_clear_pointer (&self->refresh_token, g_free);

  secret_password_clear (&satwave_schema, NULL, NULL, NULL,
                         "type", "password",
                         "account", self->username ? self->username : "",
                         NULL);
  secret_password_clear (&satwave_schema, NULL, NULL, NULL,
                         "type", "username",
                         "account", "default",
                         NULL);
}

gboolean
satwave_auth_is_authenticated (SatwaveAuth *self)
{
  return self->authenticated;
}

const char *
satwave_auth_get_access_token (SatwaveAuth *self)
{
  return self->access_token;
}

SoupSession *
satwave_auth_get_session (SatwaveAuth *self)
{
  return self->session;
}

/* ── Credential storage ── */

static void
store_credentials (const char *username,
                   const char *password)
{
  g_autofree char *label = g_strdup_printf ("Satwave - %s", username);

  secret_password_store (&satwave_schema,
                         SECRET_COLLECTION_DEFAULT,
                         label, password,
                         NULL, NULL, NULL,
                         "type", "password",
                         "account", username,
                         NULL);

  secret_password_store (&satwave_schema,
                         SECRET_COLLECTION_DEFAULT,
                         "Satwave - Account", username,
                         NULL, NULL, NULL,
                         "type", "username",
                         "account", "default",
                         NULL);
}

void
satwave_auth_save_credentials (SatwaveAuth *self,
                               const char  *username,
                               const char  *password)
{
  (void)self;
  store_credentials (username, password);
}
