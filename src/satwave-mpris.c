#include "satwave-mpris.h"
#include <gio/gio.h>
#include "satwave-config.h"

static const gchar mpris_xml[] =
  "<node>"
  "  <interface name='org.mpris.MediaPlayer2'>"
  "    <method name='Raise'/>"
  "    <method name='Quit'/>"
  "    <property type='b'  name='CanQuit'             access='read'/>"
  "    <property type='b'  name='CanRaise'            access='read'/>"
  "    <property type='b'  name='HasTrackList'        access='read'/>"
  "    <property type='s'  name='Identity'            access='read'/>"
  "    <property type='s'  name='DesktopEntry'        access='read'/>"
  "    <property type='as' name='SupportedUriSchemes' access='read'/>"
  "    <property type='as' name='SupportedMimeTypes'  access='read'/>"
  "  </interface>"
  "  <interface name='org.mpris.MediaPlayer2.Player'>"
  "    <method name='Next'/>"
  "    <method name='Previous'/>"
  "    <method name='Pause'/>"
  "    <method name='PlayPause'/>"
  "    <method name='Stop'/>"
  "    <method name='Play'/>"
  "    <method name='Seek'>"
  "      <arg type='x' name='Offset' direction='in'/>"
  "    </method>"
  "    <method name='SetPosition'>"
  "      <arg type='o' name='TrackId'  direction='in'/>"
  "      <arg type='x' name='Position' direction='in'/>"
  "    </method>"
  "    <method name='OpenUri'>"
  "      <arg type='s' name='Uri' direction='in'/>"
  "    </method>"
  "    <property type='s'     name='PlaybackStatus' access='read'/>"
  "    <property type='s'     name='LoopStatus'     access='readwrite'/>"
  "    <property type='d'     name='Rate'           access='readwrite'/>"
  "    <property type='b'     name='Shuffle'        access='readwrite'/>"
  "    <property type='a{sv}' name='Metadata'       access='read'/>"
  "    <property type='d'     name='Volume'         access='readwrite'/>"
  "    <property type='x'     name='Position'       access='read'/>"
  "    <property type='d'     name='MinimumRate'    access='read'/>"
  "    <property type='d'     name='MaximumRate'    access='read'/>"
  "    <property type='b'     name='CanGoNext'      access='read'/>"
  "    <property type='b'     name='CanGoPrevious'  access='read'/>"
  "    <property type='b'     name='CanPlay'        access='read'/>"
  "    <property type='b'     name='CanPause'       access='read'/>"
  "    <property type='b'     name='CanSeek'        access='read'/>"
  "    <property type='b'     name='CanControl'     access='read'/>"
  "  </interface>"
  "</node>";

struct _SatwaveMpris {
  GObject          parent_instance;

  SatwavePlayer   *player;
  GDBusConnection *connection;
  GDBusNodeInfo   *node_info;
  guint            root_reg_id;
  guint            player_reg_id;
  guint            name_owner_id;

  char            *title;
  char            *artist;
  char            *channel_name;
  char            *art_url;
  gboolean         playing;
};

G_DEFINE_TYPE (SatwaveMpris, satwave_mpris, G_TYPE_OBJECT)

/* ── Property getter ── */

static GVariant *
build_metadata (SatwaveMpris *self)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&b, "{sv}", "mpris:trackid",
    g_variant_new_object_path ("/org/mpris/MediaPlayer2/TrackList/NoTrack"));

  if (self->title && *self->title)
    g_variant_builder_add (&b, "{sv}", "xesam:title",
      g_variant_new_string (self->title));

  if (self->artist && *self->artist) {
    const char *artists[] = { self->artist, NULL };
    g_variant_builder_add (&b, "{sv}", "xesam:artist",
      g_variant_new_strv (artists, -1));
  }

  if (self->channel_name && *self->channel_name)
    g_variant_builder_add (&b, "{sv}", "xesam:album",
      g_variant_new_string (self->channel_name));

  if (self->art_url && *self->art_url)
    g_variant_builder_add (&b, "{sv}", "mpris:artUrl",
      g_variant_new_string (self->art_url));

  return g_variant_builder_end (&b);
}

static GVariant *
handle_get_property (GDBusConnection *conn,
                     const char      *sender,
                     const char      *obj_path,
                     const char      *iface,
                     const char      *prop,
                     GError         **error,
                     gpointer         user_data)
{
  SatwaveMpris *self = SATWAVE_MPRIS (user_data);
  (void)conn; (void)sender; (void)obj_path;

  /* org.mpris.MediaPlayer2 */
  if (g_strcmp0 (iface, "org.mpris.MediaPlayer2") == 0) {
    if (g_strcmp0 (prop, "CanQuit")      == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanRaise")     == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "HasTrackList") == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "Identity")     == 0) return g_variant_new_string ("Satwave");
    if (g_strcmp0 (prop, "DesktopEntry") == 0) return g_variant_new_string (APP_ID);
    if (g_strcmp0 (prop, "SupportedUriSchemes") == 0)
      return g_variant_new_strv ((const char *[]){ NULL }, 0);
    if (g_strcmp0 (prop, "SupportedMimeTypes") == 0)
      return g_variant_new_strv ((const char *[]){ NULL }, 0);
  }

  /* org.mpris.MediaPlayer2.Player */
  if (g_strcmp0 (iface, "org.mpris.MediaPlayer2.Player") == 0) {
    if (g_strcmp0 (prop, "PlaybackStatus") == 0)
      return g_variant_new_string (self->playing ? "Playing" : "Paused");
    if (g_strcmp0 (prop, "LoopStatus")     == 0) return g_variant_new_string ("None");
    if (g_strcmp0 (prop, "Rate")           == 0) return g_variant_new_double (1.0);
    if (g_strcmp0 (prop, "Shuffle")        == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "Metadata")       == 0) return build_metadata (self);
    if (g_strcmp0 (prop, "Volume")         == 0)
      return g_variant_new_double (satwave_player_get_volume (self->player));
    if (g_strcmp0 (prop, "Position")       == 0) return g_variant_new_int64 (0);
    if (g_strcmp0 (prop, "MinimumRate")    == 0) return g_variant_new_double (1.0);
    if (g_strcmp0 (prop, "MaximumRate")    == 0) return g_variant_new_double (1.0);
    if (g_strcmp0 (prop, "CanGoNext")      == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "CanGoPrevious")  == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "CanPlay")        == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanPause")       == 0) return g_variant_new_boolean (TRUE);
    if (g_strcmp0 (prop, "CanSeek")        == 0) return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (prop, "CanControl")     == 0) return g_variant_new_boolean (TRUE);
  }

  return NULL;
}

/* ── Property setter ── */

static gboolean
handle_set_property (GDBusConnection *conn,
                     const char      *sender,
                     const char      *obj_path,
                     const char      *iface,
                     const char      *prop,
                     GVariant        *value,
                     GError         **error,
                     gpointer         user_data)
{
  SatwaveMpris *self = SATWAVE_MPRIS (user_data);
  (void)conn; (void)sender; (void)obj_path; (void)iface; (void)error;

  if (g_strcmp0 (prop, "Volume") == 0) {
    satwave_player_set_volume (self->player, g_variant_get_double (value));
    return TRUE;
  }

  return TRUE;
}

/* ── Method handler ── */

static void
handle_method_call (GDBusConnection       *conn,
                    const char            *sender,
                    const char            *obj_path,
                    const char            *iface,
                    const char            *method,
                    GVariant              *params,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  SatwaveMpris *self = SATWAVE_MPRIS (user_data);
  (void)conn; (void)sender; (void)obj_path; (void)params;

  if (g_strcmp0 (method, "PlayPause") == 0) {
    if (satwave_player_is_playing (self->player))
      satwave_player_pause (self->player);
    else
      satwave_player_resume (self->player);
  } else if (g_strcmp0 (method, "Play") == 0) {
    satwave_player_resume (self->player);
  } else if (g_strcmp0 (method, "Pause") == 0) {
    satwave_player_pause (self->player);
  } else if (g_strcmp0 (method, "Stop") == 0) {
    satwave_player_stop (self->player);
  } else if (g_strcmp0 (method, "Quit") == 0) {
    GApplication *app = g_application_get_default ();
    if (app)
      g_application_quit (app);
  }

  g_dbus_method_invocation_return_value (invocation, NULL);
}

/* ── Emit PropertiesChanged ── */

static void
emit_properties_changed (SatwaveMpris *self,
                         const char   *iface,
                         const char   *first_prop, ...)
{
  if (!self->connection)
    return;

  GVariantBuilder changed, invalidated;
  g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_init (&invalidated, G_VARIANT_TYPE ("as"));

  va_list ap;
  va_start (ap, first_prop);
  const char *prop = first_prop;
  while (prop) {
    GVariant *val = handle_get_property (self->connection, NULL,
                                         "/org/mpris/MediaPlayer2",
                                         iface, prop, NULL, self);
    if (val)
      g_variant_builder_add (&changed, "{sv}", prop, val);
    prop = va_arg (ap, const char *);
  }
  va_end (ap);

  g_dbus_connection_emit_signal (
    self->connection, NULL,
    "/org/mpris/MediaPlayer2",
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged",
    g_variant_new ("(sa{sv}as)", iface, &changed, &invalidated),
    NULL);
}

/* ── Bus acquired ── */

static const GDBusInterfaceVTable vtable = {
  handle_method_call,
  handle_get_property,
  handle_set_property
};

static void
on_bus_acquired (GDBusConnection *conn,
                 const char      *name,
                 gpointer         user_data)
{
  SatwaveMpris *self = SATWAVE_MPRIS (user_data);
  self->connection = g_object_ref (conn);

  self->root_reg_id = g_dbus_connection_register_object (
    conn, "/org/mpris/MediaPlayer2",
    self->node_info->interfaces[0], &vtable, self, NULL, NULL);

  self->player_reg_id = g_dbus_connection_register_object (
    conn, "/org/mpris/MediaPlayer2",
    self->node_info->interfaces[1], &vtable, self, NULL, NULL);

  (void)name;
}

/* ── GObject lifecycle ── */

static void
satwave_mpris_dispose (GObject *object)
{
  SatwaveMpris *self = SATWAVE_MPRIS (object);

  if (self->name_owner_id > 0) {
    g_bus_unown_name (self->name_owner_id);
    self->name_owner_id = 0;
  }

  if (self->connection) {
    if (self->root_reg_id > 0)
      g_dbus_connection_unregister_object (self->connection, self->root_reg_id);
    if (self->player_reg_id > 0)
      g_dbus_connection_unregister_object (self->connection, self->player_reg_id);
    g_clear_object (&self->connection);
  }

  g_clear_pointer (&self->node_info, g_dbus_node_info_unref);
  g_clear_object (&self->player);
  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->artist, g_free);
  g_clear_pointer (&self->channel_name, g_free);
  g_clear_pointer (&self->art_url, g_free);

  G_OBJECT_CLASS (satwave_mpris_parent_class)->dispose (object);
}

static void
satwave_mpris_class_init (SatwaveMprisClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_mpris_dispose;
}

static void
satwave_mpris_init (SatwaveMpris *self)
{
  (void)self;
}

SatwaveMpris *
satwave_mpris_new (SatwavePlayer *player)
{
  SatwaveMpris *self = g_object_new (SATWAVE_TYPE_MPRIS, NULL);
  self->player = g_object_ref (player);

  self->node_info = g_dbus_node_info_new_for_xml (mpris_xml, NULL);
  g_assert (self->node_info != NULL);

  self->name_owner_id = g_bus_own_name (
    G_BUS_TYPE_SESSION,
    "org.mpris.MediaPlayer2.satwave",
    G_BUS_NAME_OWNER_FLAGS_NONE,
    on_bus_acquired, NULL, NULL,
    self, NULL);

  return self;
}

void
satwave_mpris_set_metadata (SatwaveMpris *self,
                            const char   *title,
                            const char   *artist,
                            const char   *channel_name,
                            const char   *art_url)
{
  /* Skip the D-Bus broadcast when nothing changed — GStreamer re-emits
   * identical tags on HLS segment boundaries */
  if (g_strcmp0 (self->title, title) == 0 &&
      g_strcmp0 (self->artist, artist) == 0 &&
      g_strcmp0 (self->channel_name, channel_name) == 0 &&
      g_strcmp0 (self->art_url, art_url) == 0)
    return;

  g_free (self->title);
  g_free (self->artist);
  g_free (self->channel_name);
  g_free (self->art_url);
  self->title = g_strdup (title);
  self->artist = g_strdup (artist);
  self->channel_name = g_strdup (channel_name);
  self->art_url = g_strdup (art_url);

  emit_properties_changed (self,
    "org.mpris.MediaPlayer2.Player",
    "Metadata", NULL);
}

void
satwave_mpris_set_playback_status (SatwaveMpris *self,
                                   gboolean      playing)
{
  if (self->playing == playing)
    return;

  self->playing = playing;

  emit_properties_changed (self,
    "org.mpris.MediaPlayer2.Player",
    "PlaybackStatus", NULL);
}
