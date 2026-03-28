#include "satwave-login-dialog.h"

struct _SatwaveLoginDialog {
  AdwDialog    parent_instance;

  SatwaveAuth *auth;

  GtkWidget   *username_row;
  GtkWidget   *password_row;
  GtkWidget   *login_button;
  GtkWidget   *spinner;
  GtkWidget   *error_label;
};

enum {
  SIGNAL_LOGIN_SUCCESS,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (SatwaveLoginDialog, satwave_login_dialog, ADW_TYPE_DIALOG)

static void
on_login_complete (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  SatwaveLoginDialog *self = SATWAVE_LOGIN_DIALOG (user_data);
  g_autoptr (GError) error = NULL;

  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_sensitive (self->login_button, TRUE);
  gtk_widget_set_sensitive (self->username_row, TRUE);
  gtk_widget_set_sensitive (self->password_row, TRUE);

  if (!satwave_auth_login_finish (self->auth, result, &error)) {
    gtk_label_set_text (GTK_LABEL (self->error_label), error->message);
    gtk_widget_set_visible (self->error_label, TRUE);
    return;
  }

  /* Save credentials */
  const char *username = gtk_editable_get_text (GTK_EDITABLE (self->username_row));
  const char *password = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
  satwave_auth_save_credentials (self->auth, username, password);

  gtk_widget_set_visible (self->error_label, FALSE);
  g_signal_emit (self, signals[SIGNAL_LOGIN_SUCCESS], 0);
  adw_dialog_close (ADW_DIALOG (self));
}

static void
on_login_clicked (GtkButton *button,
                  gpointer   user_data)
{
  SatwaveLoginDialog *self = SATWAVE_LOGIN_DIALOG (user_data);

  const char *username = gtk_editable_get_text (GTK_EDITABLE (self->username_row));
  const char *password = gtk_editable_get_text (GTK_EDITABLE (self->password_row));

  if (!username || !*username || !password || !*password) {
    gtk_label_set_text (GTK_LABEL (self->error_label), "Please enter both email and password");
    gtk_widget_set_visible (self->error_label, TRUE);
    return;
  }

  gtk_widget_set_visible (self->error_label, FALSE);
  gtk_widget_set_visible (self->spinner, TRUE);
  gtk_widget_set_sensitive (self->login_button, FALSE);
  gtk_widget_set_sensitive (self->username_row, FALSE);
  gtk_widget_set_sensitive (self->password_row, FALSE);

  satwave_auth_login_async (self->auth, username, password,
                            NULL, on_login_complete, self);
}

static void
satwave_login_dialog_dispose (GObject *object)
{
  SatwaveLoginDialog *self = SATWAVE_LOGIN_DIALOG (object);
  g_clear_object (&self->auth);
  G_OBJECT_CLASS (satwave_login_dialog_parent_class)->dispose (object);
}

static void
satwave_login_dialog_class_init (SatwaveLoginDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_login_dialog_dispose;

  signals[SIGNAL_LOGIN_SUCCESS] =
    g_signal_new ("login-success",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
satwave_login_dialog_init (SatwaveLoginDialog *self)
{
}

SatwaveLoginDialog *
satwave_login_dialog_new (SatwaveAuth *auth)
{
  SatwaveLoginDialog *self = g_object_new (SATWAVE_TYPE_LOGIN_DIALOG,
                                            "title", "Sign In",
                                            "content-width", 360,
                                            "content-height", 420,
                                            NULL);
  self->auth = g_object_ref (auth);

  /* Build the UI programmatically */
  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 360);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 24);
  gtk_widget_set_margin_bottom (box, 24);

  /* Logo / title area */
  GtkWidget *title = gtk_label_new ("SiriusXM");
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *subtitle = gtk_label_new ("Sign in with your SiriusXM account");
  gtk_widget_add_css_class (subtitle, "dim-label");
  gtk_box_append (GTK_BOX (box), subtitle);

  /* Credentials group */
  GtkWidget *prefs_group = adw_preferences_group_new ();

  self->username_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->username_row), "Email");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (prefs_group), self->username_row);

  self->password_row = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->password_row), "Password");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (prefs_group), self->password_row);

  gtk_box_append (GTK_BOX (box), prefs_group);

  /* Error label */
  self->error_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (self->error_label, "error");
  gtk_widget_set_visible (self->error_label, FALSE);
  gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
  gtk_box_append (GTK_BOX (box), self->error_label);

  /* Spinner */
  self->spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (self->spinner), TRUE);
  gtk_widget_set_visible (self->spinner, FALSE);
  gtk_widget_set_halign (self->spinner, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), self->spinner);

  /* Login button */
  self->login_button = gtk_button_new_with_label ("Sign In");
  gtk_widget_add_css_class (self->login_button, "suggested-action");
  gtk_widget_add_css_class (self->login_button, "pill");
  gtk_widget_set_halign (self->login_button, GTK_ALIGN_CENTER);
  g_signal_connect (self->login_button, "clicked", G_CALLBACK (on_login_clicked), self);
  gtk_box_append (GTK_BOX (box), self->login_button);

  /* Also trigger login on Enter in password field */
  g_signal_connect_swapped (self->password_row, "entry-activated",
                            G_CALLBACK (gtk_widget_activate), self->login_button);

  adw_clamp_set_child (ADW_CLAMP (clamp), box);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
  adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

  return self;
}
