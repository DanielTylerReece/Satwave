#include "satwave-tray.h"
#include <gio/gio.h>
#include "satwave-config.h"

/*
 * System tray icon via the StatusNotifierItem D-Bus spec (org.kde.StatusNotifierItem)
 * with a minimal com.canonical.dbusmenu for the context menu. Implemented directly
 * over GDBus — the appindicator libraries link GTK3 and can't be loaded into a
 * GTK4 process.
 *
 * The item lives on its own private bus connection so that destroying the tray
 * closes the connection and the StatusNotifierWatcher drops the icon immediately
 * (the watcher tracks items by bus name lifetime, not object registration).
 */

#define SNI_OBJECT_PATH  "/StatusNotifierItem"
#define MENU_OBJECT_PATH "/StatusNotifierItem/Menu"

#define MENU_ITEM_SHOW 1
#define MENU_ITEM_QUIT 2

static const gchar sni_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <method name='ContextMenu'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='Activate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='Scroll'>"
  "      <arg type='i' name='delta' direction='in'/>"
  "      <arg type='s' name='orientation' direction='in'/>"
  "    </method>"
  "    <property type='s' name='Category'      access='read'/>"
  "    <property type='s' name='Id'            access='read'/>"
  "    <property type='s' name='Title'         access='read'/>"
  "    <property type='s' name='Status'        access='read'/>"
  "    <property type='u' name='WindowId'      access='read'/>"
  "    <property type='s' name='IconName'      access='read'/>"
  "    <property type='s' name='IconThemePath' access='read'/>"
  "    <property type='o' name='Menu'          access='read'/>"
  "    <property type='b' name='ItemIsMenu'    access='read'/>"
  "    <property type='(sa(iiay)ss)' name='ToolTip' access='read'/>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewToolTip'/>"
  "    <signal name='NewStatus'>"
  "      <arg type='s' name='status'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

static const gchar menu_xml[] =
  "<node>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <method name='GetLayout'>"
  "      <arg type='i'  name='parentId'       direction='in'/>"
  "      <arg type='i'  name='recursionDepth' direction='in'/>"
  "      <arg type='as' name='propertyNames'  direction='in'/>"
  "      <arg type='u'  name='revision'       direction='out'/>"
  "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' name='ids'           direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
  "    </method>"
  "    <method name='GetProperty'>"
  "      <arg type='i' name='id'    direction='in'/>"
  "      <arg type='s' name='name'  direction='in'/>"
  "      <arg type='v' name='value' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' name='id'        direction='in'/>"
  "      <arg type='s' name='eventId'   direction='in'/>"
  "      <arg type='v' name='data'      direction='in'/>"
  "      <arg type='u' name='timestamp' direction='in'/>"
  "    </method>"
  "    <method name='EventGroup'>"
  "      <arg type='a(isvu)' name='events'   direction='in'/>"
  "      <arg type='ai'      name='idErrors' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' name='id'         direction='in'/>"
  "      <arg type='b' name='needUpdate' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShowGroup'>"
  "      <arg type='ai' name='ids'           direction='in'/>"
  "      <arg type='ai' name='updatesNeeded' direction='out'/>"
  "      <arg type='ai' name='idErrors'      direction='out'/>"
  "    </method>"
  "    <property type='u'  name='Version'       access='read'/>"
  "    <property type='s'  name='TextDirection' access='read'/>"
  "    <property type='s'  name='Status'        access='read'/>"
  "    <property type='as' name='IconThemePath' access='read'/>"
  "    <signal name='ItemsPropertiesUpdated'>"
  "      <arg type='a(ia{sv})' name='updatedProps'/>"
  "      <arg type='a(ias)'    name='removedProps'/>"
  "    </signal>"
  "    <signal name='LayoutUpdated'>"
  "      <arg type='u' name='revision'/>"
  "      <arg type='i' name='parent'/>"
  "    </signal>"
  "    <signal name='ItemActivationRequested'>"
  "      <arg type='i' name='id'/>"
  "      <arg type='u' name='timestamp'/>"
  "    </signal>"
  "  </interface>"
  "</node>";

struct _SatwaveTray {
  GObject          parent_instance;

  GDBusConnection *connection;       /* private session-bus connection */
  GDBusNodeInfo   *sni_node_info;
  GDBusNodeInfo   *menu_node_info;
  guint            sni_reg_id;
  guint            menu_reg_id;
  guint            watcher_watch_id;
};

G_DEFINE_TYPE (SatwaveTray, satwave_tray, G_TYPE_OBJECT)

enum {
  SIGNAL_ACTIVATE,
  SIGNAL_SHOW_WINDOW,
  SIGNAL_QUIT,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ── StatusNotifierItem interface ── */

static GVariant *
sni_get_property (GDBusConnection *conn,
                  const char      *sender,
                  const char      *obj_path,
                  const char      *iface,
                  const char      *prop,
                  GError         **error,
                  gpointer         user_data)
{
  (void)conn; (void)sender; (void)obj_path; (void)iface; (void)error; (void)user_data;

  if (g_strcmp0 (prop, "Category")      == 0) return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (prop, "Id")            == 0) return g_variant_new_string (APP_ID);
  if (g_strcmp0 (prop, "Title")         == 0) return g_variant_new_string ("Satwave");
  if (g_strcmp0 (prop, "Status")        == 0) return g_variant_new_string ("Active");
  if (g_strcmp0 (prop, "WindowId")      == 0) return g_variant_new_uint32 (0);
  if (g_strcmp0 (prop, "IconName")      == 0) return g_variant_new_string (APP_ID);
  if (g_strcmp0 (prop, "IconThemePath") == 0) return g_variant_new_string ("");
  if (g_strcmp0 (prop, "Menu")          == 0) return g_variant_new_object_path (MENU_OBJECT_PATH);
  if (g_strcmp0 (prop, "ItemIsMenu")    == 0) return g_variant_new_boolean (FALSE);
  if (g_strcmp0 (prop, "ToolTip")       == 0)
    return g_variant_new ("(s@a(iiay)ss)", APP_ID,
                          g_variant_new_array (G_VARIANT_TYPE ("(iiay)"), NULL, 0),
                          "Satwave", "");

  return NULL;
}

static void
sni_method_call (GDBusConnection       *conn,
                 const char            *sender,
                 const char            *obj_path,
                 const char            *iface,
                 const char            *method,
                 GVariant              *params,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  SatwaveTray *self = SATWAVE_TRAY (user_data);
  (void)conn; (void)sender; (void)obj_path; (void)iface; (void)params;

  if (g_strcmp0 (method, "Activate") == 0 ||
      g_strcmp0 (method, "SecondaryActivate") == 0)
    g_signal_emit (self, signals[SIGNAL_ACTIVATE], 0);

  /* ContextMenu is handled by the host via the Menu property; Scroll is ignored */

  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable sni_vtable = {
  sni_method_call,
  sni_get_property,
  NULL
};

/* ── com.canonical.dbusmenu interface ── */

static GVariant *
menu_item_properties (int id)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));

  switch (id) {
  case 0:
    g_variant_builder_add (&b, "{sv}", "children-display",
                           g_variant_new_string ("submenu"));
    break;
  case MENU_ITEM_SHOW:
    g_variant_builder_add (&b, "{sv}", "label", g_variant_new_string ("Show Satwave"));
    break;
  case MENU_ITEM_QUIT:
    g_variant_builder_add (&b, "{sv}", "label", g_variant_new_string ("Quit"));
    break;
  }

  g_variant_builder_add (&b, "{sv}", "enabled", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&b, "{sv}", "visible", g_variant_new_boolean (TRUE));

  return g_variant_builder_end (&b);
}

static GVariant *
menu_item_layout (int id, gboolean with_children)
{
  GVariantBuilder children;
  g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

  if (with_children && id == 0) {
    g_variant_builder_add (&children, "v", menu_item_layout (MENU_ITEM_SHOW, FALSE));
    g_variant_builder_add (&children, "v", menu_item_layout (MENU_ITEM_QUIT, FALSE));
  }

  return g_variant_new ("(i@a{sv}av)", id, menu_item_properties (id), &children);
}

static void
menu_handle_event (SatwaveTray *self,
                   gint32       id,
                   const char  *event_id)
{
  if (g_strcmp0 (event_id, "clicked") != 0)
    return;

  if (id == MENU_ITEM_SHOW)
    g_signal_emit (self, signals[SIGNAL_SHOW_WINDOW], 0);
  else if (id == MENU_ITEM_QUIT)
    g_signal_emit (self, signals[SIGNAL_QUIT], 0);
}

static void
menu_method_call (GDBusConnection       *conn,
                  const char            *sender,
                  const char            *obj_path,
                  const char            *iface,
                  const char            *method,
                  GVariant              *params,
                  GDBusMethodInvocation *invocation,
                  gpointer               user_data)
{
  SatwaveTray *self = SATWAVE_TRAY (user_data);
  (void)conn; (void)sender; (void)obj_path; (void)iface;

  if (g_strcmp0 (method, "GetLayout") == 0) {
    gint32 parent_id;
    g_variant_get (params, "(ii@as)", &parent_id, NULL, NULL);
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(u@(ia{sv}av))", 1,
                     menu_item_layout (parent_id, parent_id == 0)));

  } else if (g_strcmp0 (method, "GetGroupProperties") == 0) {
    GVariantBuilder result;
    g_variant_builder_init (&result, G_VARIANT_TYPE ("a(ia{sv})"));
    g_autoptr (GVariantIter) iter = NULL;
    gint32 id;
    g_variant_get (params, "(ai@as)", &iter, NULL);
    while (g_variant_iter_next (iter, "i", &id))
      g_variant_builder_add (&result, "(i@a{sv})", id, menu_item_properties (id));
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(a(ia{sv}))", &result));

  } else if (g_strcmp0 (method, "GetProperty") == 0) {
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(v)", g_variant_new_string ("")));

  } else if (g_strcmp0 (method, "Event") == 0) {
    gint32 id;
    const char *event_id;
    g_variant_get (params, "(i&svu)", &id, &event_id, NULL, NULL);
    menu_handle_event (self, id, event_id);
    g_dbus_method_invocation_return_value (invocation, NULL);

  } else if (g_strcmp0 (method, "EventGroup") == 0) {
    g_autoptr (GVariantIter) iter = NULL;
    gint32 id;
    const char *event_id;
    g_variant_get (params, "(a(isvu))", &iter);
    while (g_variant_iter_next (iter, "(i&svu)", &id, &event_id, NULL, NULL))
      menu_handle_event (self, id, event_id);
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(ai)", NULL));

  } else if (g_strcmp0 (method, "AboutToShow") == 0) {
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(b)", FALSE));

  } else if (g_strcmp0 (method, "AboutToShowGroup") == 0) {
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(aiai)", NULL, NULL));
  }
}

static GVariant *
menu_get_property (GDBusConnection *conn,
                   const char      *sender,
                   const char      *obj_path,
                   const char      *iface,
                   const char      *prop,
                   GError         **error,
                   gpointer         user_data)
{
  (void)conn; (void)sender; (void)obj_path; (void)iface; (void)error; (void)user_data;

  if (g_strcmp0 (prop, "Version")       == 0) return g_variant_new_uint32 (3);
  if (g_strcmp0 (prop, "TextDirection") == 0) return g_variant_new_string ("ltr");
  if (g_strcmp0 (prop, "Status")        == 0) return g_variant_new_string ("normal");
  if (g_strcmp0 (prop, "IconThemePath") == 0)
    return g_variant_new_strv (NULL, 0);

  return NULL;
}

static const GDBusInterfaceVTable menu_vtable = {
  menu_method_call,
  menu_get_property,
  NULL
};

/* ── Watcher registration ── */

static void
on_register_done (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = NULL;
  (void)user_data;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (error)
    g_warning ("Failed to register status notifier item: %s", error->message);
}

static void
on_watcher_appeared (GDBusConnection *conn,
                     const char      *name,
                     const char      *name_owner,
                     gpointer         user_data)
{
  (void)name; (void)name_owner; (void)user_data;

  g_dbus_connection_call (conn,
                          "org.kde.StatusNotifierWatcher",
                          "/StatusNotifierWatcher",
                          "org.kde.StatusNotifierWatcher",
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", g_dbus_connection_get_unique_name (conn)),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1, NULL,
                          on_register_done, NULL);
}

/* ── GObject lifecycle ── */

static void
satwave_tray_dispose (GObject *object)
{
  SatwaveTray *self = SATWAVE_TRAY (object);

  if (self->watcher_watch_id > 0) {
    g_bus_unwatch_name (self->watcher_watch_id);
    self->watcher_watch_id = 0;
  }

  if (self->connection) {
    if (self->sni_reg_id > 0)
      g_dbus_connection_unregister_object (self->connection, self->sni_reg_id);
    if (self->menu_reg_id > 0)
      g_dbus_connection_unregister_object (self->connection, self->menu_reg_id);
    self->sni_reg_id = 0;
    self->menu_reg_id = 0;

    /* Closing the private connection drops our bus name, which makes the
     * watcher remove the tray icon. */
    g_dbus_connection_close (self->connection, NULL, NULL, NULL);
    g_clear_object (&self->connection);
  }

  g_clear_pointer (&self->sni_node_info, g_dbus_node_info_unref);
  g_clear_pointer (&self->menu_node_info, g_dbus_node_info_unref);

  G_OBJECT_CLASS (satwave_tray_parent_class)->dispose (object);
}

static void
satwave_tray_class_init (SatwaveTrayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_tray_dispose;

  signals[SIGNAL_ACTIVATE] =
    g_signal_new ("activate", SATWAVE_TYPE_TRAY,
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_SHOW_WINDOW] =
    g_signal_new ("show-window", SATWAVE_TYPE_TRAY,
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_QUIT] =
    g_signal_new ("quit", SATWAVE_TYPE_TRAY,
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
satwave_tray_init (SatwaveTray *self)
{
  (void)self;
}

SatwaveTray *
satwave_tray_new (void)
{
  g_autoptr (GError) error = NULL;

  g_autofree char *address =
    g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!address) {
    g_warning ("Tray: failed to get session bus address: %s", error->message);
    return NULL;
  }

  g_autoptr (GDBusConnection) connection =
    g_dbus_connection_new_for_address_sync (
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  if (!connection) {
    g_warning ("Tray: failed to connect to session bus: %s", error->message);
    return NULL;
  }

  SatwaveTray *self = g_object_new (SATWAVE_TYPE_TRAY, NULL);
  self->connection = g_steal_pointer (&connection);

  self->sni_node_info = g_dbus_node_info_new_for_xml (sni_xml, NULL);
  self->menu_node_info = g_dbus_node_info_new_for_xml (menu_xml, NULL);
  g_assert (self->sni_node_info != NULL);
  g_assert (self->menu_node_info != NULL);

  self->sni_reg_id = g_dbus_connection_register_object (
    self->connection, SNI_OBJECT_PATH,
    self->sni_node_info->interfaces[0], &sni_vtable, self, NULL, &error);
  if (self->sni_reg_id == 0) {
    g_warning ("Tray: failed to export StatusNotifierItem: %s", error->message);
    g_object_unref (self);
    return NULL;
  }

  self->menu_reg_id = g_dbus_connection_register_object (
    self->connection, MENU_OBJECT_PATH,
    self->menu_node_info->interfaces[0], &menu_vtable, self, NULL, &error);
  if (self->menu_reg_id == 0) {
    g_warning ("Tray: failed to export dbusmenu: %s", error->message);
    g_object_unref (self);
    return NULL;
  }

  /* Register now if the watcher is up, and re-register whenever it restarts */
  self->watcher_watch_id = g_bus_watch_name_on_connection (
    self->connection,
    "org.kde.StatusNotifierWatcher",
    G_BUS_NAME_WATCHER_FLAGS_NONE,
    on_watcher_appeared, NULL,
    self, NULL);

  return self;
}
