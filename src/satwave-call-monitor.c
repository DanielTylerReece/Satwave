/*
 * Monitor PipeWire for communication streams (calls from Telegram, Teams, etc.)
 * and notify when a call starts/ends.
 *
 * Apps like Telegram, Teams, and Discord set media.role=Communication on their
 * audio streams. We watch the PipeWire registry for these nodes and pause/resume
 * playback accordingly.
 */

#include "satwave-call-monitor.h"
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <string.h>

/* ── GSource wrapper for PipeWire → GLib main loop integration ── */

typedef struct {
  GSource         base;
  struct pw_loop *pw_loop;
  gpointer        tag;
} PwGSource;

static gboolean
pw_gsource_prepare (GSource *src, gint *timeout)
{
  (void)src;
  *timeout = -1;
  return FALSE;
}

static gboolean
pw_gsource_check (GSource *src)
{
  PwGSource *s = (PwGSource *)src;
  GIOCondition cond = g_source_query_unix_fd (src, s->tag);
  return (cond & (G_IO_IN | G_IO_ERR | G_IO_HUP)) != 0;
}

static gboolean
pw_gsource_dispatch (GSource *src, GSourceFunc cb, gpointer data)
{
  (void)cb; (void)data;
  PwGSource *s = (PwGSource *)src;
  pw_loop_iterate (s->pw_loop, 0);
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs pw_gsource_funcs = {
  .prepare  = pw_gsource_prepare,
  .check    = pw_gsource_check,
  .dispatch = pw_gsource_dispatch,
};

/* ── Monitor struct ── */

struct _SatwaveCallMonitor {
  struct pw_loop      *loop;
  struct pw_context   *context;
  struct pw_core      *core;
  struct pw_registry  *registry;
  struct spa_hook      registry_listener;

  GSource             *gsource;
  GHashTable          *comm_nodes;  /* node id → 1 */

  SatwaveCallStateChangedCb cb;
  gpointer                  cb_data;
  gboolean                  call_active;
  guint                     debounce_id;
};

static gboolean debounce_call_check (gpointer user_data);

static gboolean
is_communication_role (const char *role)
{
  if (!role) return FALSE;
  return (g_ascii_strcasecmp (role, "communication") == 0 ||
          g_ascii_strcasecmp (role, "phone") == 0);
}

/* Known communication apps that may not set media.role correctly */
static gboolean
is_communication_app (const char *app_name, const char *node_name)
{
  if (!app_name && !node_name)
    return FALSE;

  const char *names[] = {
    "telegram", "Telegram", "TelegramDesktop",
    "teams", "Teams", "Microsoft Teams", "teams-for-linux",
    "discord", "Discord",
    "zoom", "Zoom",
    "skype", "Skype",
    "signal", "Signal",
    "webrtc", "WebRTC",
    "slack", "Slack",
    "whatsapp", "WhatsApp",
    "jitsi", "Jitsi",
    "mumble", "Mumble",
    "element", "Element",
    "linphone", "Linphone",
    NULL
  };

  for (int i = 0; names[i]; i++) {
    if (app_name && strcasestr (app_name, names[i]))
      return TRUE;
    if (node_name && strcasestr (node_name, names[i]))
      return TRUE;
  }

  return FALSE;
}

static void
on_global (void *data, uint32_t id, uint32_t permissions,
           const char *type, uint32_t version,
           const struct spa_dict *props)
{
  (void)permissions; (void)version;
  SatwaveCallMonitor *mon = data;

  if (strcmp (type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  if (!props)
    return;

  const char *role    = spa_dict_lookup (props, PW_KEY_MEDIA_ROLE);
  const char *class   = spa_dict_lookup (props, PW_KEY_MEDIA_CLASS);
  const char *app     = spa_dict_lookup (props, PW_KEY_APP_NAME);
  const char *node    = spa_dict_lookup (props, PW_KEY_NODE_NAME);
  const char *binary  = spa_dict_lookup (props, "application.process.binary");
  const char *icon    = spa_dict_lookup (props, "application.icon-name");

  /* Only interested in audio streams */
  if (!class || strstr (class, "Audio") == NULL)
    return;

  /* Log ALL audio stream creation for debugging */
  g_debug ("Call monitor: audio node id=%u app='%s' binary='%s' role='%s' class='%s' node='%s'",
           id, app ? app : "", binary ? binary : "",
           role ? role : "", class, node ? node : "");

  gboolean is_comm_role = is_communication_role (role);
  gboolean is_comm_app = is_communication_app (app, node) ||
                         is_communication_app (binary, icon);

  /* Detect calls by:
   * 1. Any stream with media.role = Communication/phone (immediate)
   * 2. A known communication app with ANY audio stream (debounced —
   *    notifications are short-lived, calls persist) */
  gboolean is_call = FALSE;

  if (is_comm_role) {
    is_call = TRUE;
  } else if (is_comm_app) {
    is_call = TRUE;
  }

  if (!is_call)
    return;

  g_message ("Call monitor: communication stream detected — id=%u app='%s' binary='%s' role='%s' class='%s'",
             id, app ? app : "?", binary ? binary : "?",
             role ? role : "", class ? class : "?");

  g_hash_table_insert (mon->comm_nodes, GUINT_TO_POINTER (id), GUINT_TO_POINTER (1));

  if (!mon->call_active && mon->debounce_id == 0) {
    /* Debounce: wait 3 seconds before declaring a call active.
     * Notification sounds disappear quickly; actual calls persist. */
    mon->debounce_id = g_timeout_add_seconds (3, debounce_call_check, mon);
  }
}

static gboolean
debounce_call_check (gpointer user_data)
{
  SatwaveCallMonitor *mon = user_data;
  mon->debounce_id = 0;

  /* If communication streams are still active after the debounce period, it's a real call */
  if (g_hash_table_size (mon->comm_nodes) > 0 && !mon->call_active) {
    g_message ("Call monitor: communication stream persisted — declaring call active");
    mon->call_active = TRUE;
    if (mon->cb)
      mon->cb (TRUE, mon->cb_data);
  }

  return G_SOURCE_REMOVE;
}

static void
on_global_remove (void *data, uint32_t id)
{
  SatwaveCallMonitor *mon = data;

  if (!g_hash_table_remove (mon->comm_nodes, GUINT_TO_POINTER (id)))
    return;

  g_debug ("Call monitor: communication stream removed — id=%u (%u remaining)",
           id, g_hash_table_size (mon->comm_nodes));

  if (g_hash_table_size (mon->comm_nodes) == 0) {
    /* Cancel debounce if stream disappeared quickly (was just a notification) */
    if (mon->debounce_id > 0) {
      g_source_remove (mon->debounce_id);
      mon->debounce_id = 0;
      g_debug ("Call monitor: stream disappeared before debounce — was a notification");
    }

    if (mon->call_active) {
      mon->call_active = FALSE;
      if (mon->cb)
        mon->cb (FALSE, mon->cb_data);
    }
  }
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global        = on_global,
  .global_remove = on_global_remove,
};

/* ── Public API ── */

SatwaveCallMonitor *
satwave_call_monitor_new (SatwaveCallStateChangedCb cb,
                          gpointer                  user_data)
{
  SatwaveCallMonitor *mon = g_new0 (SatwaveCallMonitor, 1);
  mon->cb = cb;
  mon->cb_data = user_data;
  mon->comm_nodes = g_hash_table_new (g_direct_hash, g_direct_equal);

  pw_init (NULL, NULL);

  mon->loop = pw_loop_new (NULL);
  mon->context = pw_context_new (mon->loop, NULL, 0);
  mon->core = pw_context_connect (mon->context, NULL, 0);

  if (!mon->core) {
    g_warning ("Call monitor: failed to connect to PipeWire");
    pw_context_destroy (mon->context);
    pw_loop_destroy (mon->loop);
    g_hash_table_destroy (mon->comm_nodes);
    g_free (mon);
    return NULL;
  }

  mon->registry = pw_core_get_registry (mon->core, PW_VERSION_REGISTRY, 0);
  spa_zero (mon->registry_listener);
  pw_registry_add_listener (mon->registry,
                            &mon->registry_listener,
                            &registry_events, mon);

  /* Attach PipeWire fd to GLib main loop */
  mon->gsource = g_source_new (&pw_gsource_funcs, sizeof (PwGSource));
  PwGSource *pws = (PwGSource *)mon->gsource;
  pws->pw_loop = mon->loop;

  pw_loop_enter (mon->loop);

  int fd = pw_loop_get_fd (mon->loop);
  pws->tag = g_source_add_unix_fd (mon->gsource, fd,
                                   G_IO_IN | G_IO_ERR | G_IO_HUP);
  g_source_attach (mon->gsource, NULL);

  g_debug ("Call monitor: watching PipeWire for communication streams");
  return mon;
}

void
satwave_call_monitor_free (SatwaveCallMonitor *mon)
{
  if (!mon) return;

  if (mon->debounce_id > 0) {
    g_source_remove (mon->debounce_id);
    mon->debounce_id = 0;
  }

  /* Stop GSource first to prevent callbacks during teardown */
  if (mon->gsource) {
    g_source_destroy (mon->gsource);
    g_source_unref (mon->gsource);
    mon->gsource = NULL;
  }

  /* Remove listener before destroying objects */
  spa_hook_remove (&mon->registry_listener);

  /* Disconnect core (this implicitly destroys the registry proxy) */
  if (mon->core) {
    pw_core_disconnect (mon->core);
    mon->core = NULL;
  }

  if (mon->loop)
    pw_loop_leave (mon->loop);

  if (mon->context) {
    pw_context_destroy (mon->context);
    mon->context = NULL;
  }

  if (mon->loop) {
    pw_loop_destroy (mon->loop);
    mon->loop = NULL;
  }

  g_hash_table_destroy (mon->comm_nodes);
  g_free (mon);
}
