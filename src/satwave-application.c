#include "satwave-application.h"
#include "satwave-window.h"
#include "satwave-tray.h"
#include "satwave-config.h"

struct _SatwaveApplication {
  AdwApplication parent_instance;

  GSettings   *settings;
  SatwaveTray *tray;
};

G_DEFINE_TYPE (SatwaveApplication, satwave_application, ADW_TYPE_APPLICATION)

static GtkWindow *
satwave_application_get_window (SatwaveApplication *self)
{
  /* gtk_application_get_active_window() returns NULL while the window is
   * hidden in the background, so walk the window list instead. */
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  for (GList *l = windows; l != NULL; l = l->next)
    if (SATWAVE_IS_WINDOW (l->data))
      return GTK_WINDOW (l->data);
  return NULL;
}

static void
satwave_application_activate (GApplication *app)
{
  GtkWindow *window;

  window = satwave_application_get_window (SATWAVE_APPLICATION (app));
  if (window == NULL)
    window = GTK_WINDOW (satwave_window_new (SATWAVE_APPLICATION (app)));

  gtk_window_present (window);
}

/* ── Tray ── */

static void
on_tray_activate (SatwaveTray *tray,
                  gpointer     user_data)
{
  SatwaveApplication *self = SATWAVE_APPLICATION (user_data);
  GtkWindow *window = satwave_application_get_window (self);
  (void)tray;

  if (window && gtk_widget_get_visible (GTK_WIDGET (window)))
    gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
  else
    g_application_activate (G_APPLICATION (self));
}

static void
on_tray_show_window (SatwaveTray *tray,
                     gpointer     user_data)
{
  (void)tray;
  g_application_activate (G_APPLICATION (user_data));
}

static void
on_tray_quit (SatwaveTray *tray,
              gpointer     user_data)
{
  (void)tray;
  g_application_quit (G_APPLICATION (user_data));
}

static void
satwave_application_update_tray (SatwaveApplication *self)
{
  gboolean enabled = g_settings_get_boolean (self->settings, "run-in-background");

  if (enabled && self->tray == NULL) {
    self->tray = satwave_tray_new ();
    if (self->tray) {
      g_signal_connect (self->tray, "activate",
                        G_CALLBACK (on_tray_activate), self);
      g_signal_connect (self->tray, "show-window",
                        G_CALLBACK (on_tray_show_window), self);
      g_signal_connect (self->tray, "quit",
                        G_CALLBACK (on_tray_quit), self);
    }
  } else if (!enabled && self->tray != NULL) {
    g_clear_object (&self->tray);

    /* Without a tray there is no way back to a hidden window */
    GtkWindow *window = satwave_application_get_window (self);
    if (window && !gtk_widget_get_visible (GTK_WIDGET (window)))
      gtk_window_present (window);
  }
}

static void
on_run_in_background_changed (GSettings  *settings,
                              const char *key,
                              gpointer    user_data)
{
  (void)settings; (void)key;
  satwave_application_update_tray (SATWAVE_APPLICATION (user_data));
}

static void
on_about_action (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  GtkWindow *window = gtk_application_get_active_window (app);

  AdwAboutDialog *about = ADW_ABOUT_DIALOG (adw_about_dialog_new ());
  adw_about_dialog_set_application_name (about, "Satwave");
  adw_about_dialog_set_application_icon (about, APP_ID);
  adw_about_dialog_set_version (about, APP_VERSION);
  adw_about_dialog_set_comments (about,
    "Native SiriusXM streaming client for GNOME\n\n"
    "Satwave is a 100% unofficial project and you use it at your own risk. "
    "It is designed to be used for personal use. It does not record but only "
    "plays music from an already licensed account. Similar to playing music "
    "over speakers from the radio directly. Using Satwave in any corporate "
    "setting, to attempt to pirate music, or to try to make a profit off your "
    "subscription may result in YOU getting in legal trouble.");
  adw_about_dialog_set_license_type (about, GTK_LICENSE_GPL_3_0);

  adw_about_dialog_add_legal_section (about,
    "Disclaimer",
    NULL,
    GTK_LICENSE_CUSTOM,
    "Satwave is a 100% unofficial project and you use it at your own risk. "
    "It is designed to be used for personal use. It does not record but only "
    "plays music from an already licensed account. Similar to playing music "
    "over speakers from the radio directly. Using Satwave in any corporate "
    "setting, to attempt to pirate music, or to try to make a profit off your "
    "subscription may result in YOU getting in legal trouble.\n\n"
    "SiriusXM is a registered trademark of Sirius XM Holdings Inc. "
    "This project is not affiliated with, endorsed by, or sponsored by "
    "Sirius XM Holdings Inc. in any way.");

  adw_dialog_present (ADW_DIALOG (about), GTK_WIDGET (window));
}

static void
on_quit_action (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  GApplication *app = G_APPLICATION (user_data);
  g_application_quit (app);
}

static void
on_preferences_action (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       user_data)
{
  SatwaveApplication *self = SATWAVE_APPLICATION (user_data);
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (self));
  (void)action; (void)parameter;

  AdwPreferencesDialog *prefs = ADW_PREFERENCES_DIALOG (adw_preferences_dialog_new ());

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_set_title (page, "General");
  adw_preferences_page_set_icon_name (page, "preferences-system-symbolic");

  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (group, "Behavior");

  GtkWidget *bg_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bg_row), "Run in Background");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (bg_row),
    "Keep playing and show a system tray icon when the window is closed");
  g_settings_bind (self->settings, "run-in-background",
                   bg_row, "active", G_SETTINGS_BIND_DEFAULT);

  adw_preferences_group_add (group, bg_row);
  adw_preferences_page_add (page, group);
  adw_preferences_dialog_add (prefs, page);

  adw_dialog_present (ADW_DIALOG (prefs), GTK_WIDGET (window));
}

static const GActionEntry app_actions[] = {
  { "about", on_about_action },
  { "preferences", on_preferences_action },
  { "quit", on_quit_action },
};

static void
satwave_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (satwave_application_parent_class)->startup (app);

  /* Register our bundled icon so GTK can find it */
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  gtk_icon_theme_add_resource_path (theme, "/com/github/satwave/icons");

  /* Also add the source data/icons dir so it works without install */
  g_autofree char *exe_path = g_file_read_link ("/proc/self/exe", NULL);
  if (exe_path) {
    g_autofree char *exe_dir = g_path_get_dirname (exe_path);
    g_autofree char *data_icons = g_build_filename (exe_dir, "..", "data", "icons", "hicolor", NULL);
    if (g_file_test (data_icons, G_FILE_TEST_IS_DIR))
      gtk_icon_theme_add_search_path (theme, data_icons);
    /* Also try relative to working directory */
    gtk_icon_theme_add_search_path (theme, "data/icons/hicolor");
  }

  gtk_window_set_default_icon_name (APP_ID);

  /* Load custom CSS */
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/com/github/satwave/style.css");
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (),
    GTK_STYLE_PROVIDER (css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                  app_actions,
                                  G_N_ELEMENTS (app_actions),
                                  app);

  const char *quit_accels[] = { "<Ctrl>q", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit", quit_accels);
  const char *prefs_accels[] = { "<Ctrl>comma", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.preferences", prefs_accels);

  SatwaveApplication *self = SATWAVE_APPLICATION (app);
  self->settings = g_settings_new (APP_ID);
  g_signal_connect (self->settings, "changed::run-in-background",
                    G_CALLBACK (on_run_in_background_changed), self);
  satwave_application_update_tray (self);
}

static void
satwave_application_dispose (GObject *object)
{
  SatwaveApplication *self = SATWAVE_APPLICATION (object);

  g_clear_object (&self->tray);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (satwave_application_parent_class)->dispose (object);
}

static void
satwave_application_class_init (SatwaveApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  object_class->dispose = satwave_application_dispose;
  app_class->activate = satwave_application_activate;
  app_class->startup = satwave_application_startup;
}

static void
satwave_application_init (SatwaveApplication *self)
{
}

SatwaveApplication *
satwave_application_new (void)
{
  return g_object_new (SATWAVE_TYPE_APPLICATION,
                       "application-id", APP_ID,
                       "flags", G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
