#include "satwave-window.h"
#include "satwave-auth.h"
#include "satwave-api-client.h"
#include "satwave-player.h"
#include "satwave-hls-proxy.h"
#include "satwave-channel.h"
#include "satwave-channel-store.h"
#include "satwave-login-dialog.h"
#include "satwave-config.h"

struct _SatwaveWindow {
  AdwApplicationWindow  parent_instance;

  /* Core objects */
  SatwaveAuth          *auth;
  SatwaveApiClient     *api;
  SatwavePlayer        *player;
  SatwaveHlsProxy      *proxy;
  SatwaveChannelStore  *store;
  GSettings            *settings;

  /* Current state */
  SatwaveChannel       *current_channel;
  guint                 session_refresh_id;
  guint                 now_playing_poll_id;

  /* Widgets — header */
  GtkWidget            *header_bar;
  GtkWidget            *search_button;
  GtkWidget            *search_bar;
  GtkWidget            *search_entry;
  GtkWidget            *view_switcher;

  /* Widgets — main content */
  GtkWidget            *main_stack;
  GtkWidget            *login_status_page;
  GtkWidget            *loading_status_page;
  GtkWidget            *content_box;
  GtkWidget            *toast_overlay;

  /* Widgets — channel grid */
  GtkWidget            *channel_scrolled;
  GtkWidget            *channel_grid;

  /* Widgets — category sidebar */
  GtkWidget            *split_view;
  GtkWidget            *category_list;

  /* Widgets — now playing bar (bottom) */
  GtkWidget            *now_playing_bar;
  GtkWidget            *np_channel_label;
  GtkWidget            *np_title_label;
  GtkWidget            *np_image;
  GtkWidget            *play_button;
  GtkWidget            *volume_button;

  /* Retro LCD display */
  GtkWidget            *lcd_frame;
  GtkWidget            *lcd_channel_label;
  GtkWidget            *lcd_title_label;
  GtkWidget            *lcd_freq_label;

  /* Widgets — navigation */
  GtkWidget            *nav_all_btn;
  GtkWidget            *nav_fav_btn;
};

G_DEFINE_TYPE (SatwaveWindow, satwave_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward declarations */
static void load_channels (SatwaveWindow *self);
static void show_login_dialog (SatwaveWindow *self);
static void update_now_playing (SatwaveWindow *self);
static void on_stream_url_ready (GObject *source, GAsyncResult *result, gpointer user_data);
static void on_favorite_toggled (GtkButton *button, gpointer user_data);
static void start_now_playing_poll (SatwaveWindow *self);

/* ── Play a channel (used from card and detail dialog) ── */

static void
play_channel (SatwaveWindow  *self,
              SatwaveChannel *channel)
{
  g_debug ("Playing channel: %s (%s)",
           satwave_channel_get_name (channel),
           satwave_channel_get_id (channel));

  g_set_object (&self->current_channel, channel);

  satwave_api_client_get_stream_url_async (
    self->api, channel, NULL,
    (GAsyncReadyCallback) on_stream_url_ready, self);
}

/* ── Channel detail dialog ── */

static void
on_channel_desc_loaded (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GtkWidget *label = GTK_WIDGET (user_data);
  SatwaveApiClient *api = SATWAVE_API_CLIENT (source);
  g_autoptr (GError) error = NULL;

  g_autofree char *desc = satwave_api_client_get_channel_desc_finish (api, result, &error);

  if (desc && *desc && GTK_IS_LABEL (label)) {
    gtk_label_set_text (GTK_LABEL (label), desc);
    gtk_widget_set_visible (label, TRUE);
  }

  g_object_unref (label);
}

static void
on_dialog_play_clicked (GtkButton *button,
                        gpointer   user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (g_object_get_data (G_OBJECT (button), "window"));
  SatwaveChannel *channel = g_object_get_data (G_OBJECT (button), "channel");
  AdwDialog *dialog = ADW_DIALOG (g_object_get_data (G_OBJECT (button), "dialog"));

  if (channel && self)
    play_channel (self, channel);

  if (dialog)
    adw_dialog_close (dialog);
}

static void
on_dialog_image_loaded (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GtkWidget *image = GTK_WIDGET (user_data);
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (!error && bytes) {
    g_autoptr (GdkTexture) texture = gdk_texture_new_from_bytes (bytes, NULL);
    if (texture && GTK_IS_IMAGE (image)) {
      gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
      gtk_image_set_pixel_size (GTK_IMAGE (image), 240);
    }
  }

  g_object_unref (image);
}

static void
show_channel_detail (SatwaveWindow  *self,
                     SatwaveChannel *channel)
{
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, satwave_channel_get_name (channel));
  adw_dialog_set_content_width (dialog, 380);
  adw_dialog_set_content_height (dialog, 520);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 360);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_top (box, 16);
  gtk_widget_set_margin_bottom (box, 24);

  /* Channel artwork — large */
  GtkWidget *image = gtk_image_new_from_icon_name ("audio-x-generic-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (image), 240);
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (image, "icon-dropshadow");
  gtk_box_append (GTK_BOX (box), image);

  /* Load artwork async */
  const char *img_url = satwave_channel_get_image_url (channel);
  if (img_url && *img_url) {
    SoupMessage *img_msg = soup_message_new (SOUP_METHOD_GET, img_url);
    if (img_msg) {
      g_object_ref (image);
      soup_session_send_and_read_async (
        satwave_auth_get_session (self->auth), img_msg,
        G_PRIORITY_DEFAULT, NULL,
        (GAsyncReadyCallback) on_dialog_image_loaded, image);
      g_object_unref (img_msg);
    }
  }

  /* Channel number + category */
  g_autofree char *subtitle = g_strdup_printf ("CH %03d  •  %s",
    satwave_channel_get_number (channel),
    satwave_channel_get_category (channel));
  GtkWidget *subtitle_label = gtk_label_new (subtitle);
  gtk_widget_add_css_class (subtitle_label, "dim-label");
  gtk_widget_add_css_class (subtitle_label, "caption-heading");
  gtk_widget_set_halign (subtitle_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), subtitle_label);

  /* Channel name */
  GtkWidget *name_label = gtk_label_new (satwave_channel_get_name (channel));
  gtk_widget_add_css_class (name_label, "title-1");
  gtk_widget_set_halign (name_label, GTK_ALIGN_CENTER);
  gtk_label_set_wrap (GTK_LABEL (name_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (name_label), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), name_label);

  /* Short description (byline from API) */
  const char *desc = satwave_channel_get_description (channel);
  if (desc && *desc) {
    GtkWidget *desc_label = gtk_label_new (desc);
    gtk_widget_add_css_class (desc_label, "body");
    gtk_widget_add_css_class (desc_label, "dim-label");
    gtk_label_set_wrap (GTK_LABEL (desc_label), TRUE);
    gtk_label_set_justify (GTK_LABEL (desc_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign (desc_label, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), desc_label);
  }

  /* Long description (fetched from siriusxm.com) */
  GtkWidget *long_desc_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (long_desc_label, "body");
  gtk_label_set_wrap (GTK_LABEL (long_desc_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (long_desc_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (long_desc_label, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (long_desc_label, FALSE);
  gtk_box_append (GTK_BOX (box), long_desc_label);

  /* Fetch long description async */
  g_object_ref (long_desc_label);
  satwave_api_client_get_channel_desc_async (
    self->api, satwave_channel_get_name (channel), NULL,
    (GAsyncReadyCallback) on_channel_desc_loaded, long_desc_label);

  /* Spacer */
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (box), spacer);

  /* Button row */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (btn_box, 8);

  /* Play button */
  GtkWidget *play_btn = gtk_button_new_with_label ("Play");
  gtk_button_set_icon_name (GTK_BUTTON (play_btn), "media-playback-start-symbolic");
  gtk_widget_add_css_class (play_btn, "suggested-action");
  gtk_widget_add_css_class (play_btn, "pill");
  g_object_set_data_full (G_OBJECT (play_btn), "channel",
                          g_object_ref (channel), g_object_unref);
  g_object_set_data (G_OBJECT (play_btn), "window", self);
  g_object_set_data (G_OBJECT (play_btn), "dialog", dialog);
  g_signal_connect (play_btn, "clicked", G_CALLBACK (on_dialog_play_clicked), NULL);
  gtk_box_append (GTK_BOX (btn_box), play_btn);

  /* Favorite toggle */
  gboolean is_fav = satwave_channel_get_is_favorite (channel);
  const char *fav_icon = is_fav ? "starred-symbolic" : "non-starred-symbolic";
  GtkWidget *fav_btn = gtk_button_new_from_icon_name (fav_icon);
  gtk_widget_add_css_class (fav_btn, "circular");
  g_object_set_data_full (G_OBJECT (fav_btn), "channel",
                          g_object_ref (channel), g_object_unref);
  g_signal_connect (fav_btn, "clicked", G_CALLBACK (on_favorite_toggled), self);
  gtk_box_append (GTK_BOX (btn_box), fav_btn);

  gtk_box_append (GTK_BOX (box), btn_box);

  adw_clamp_set_child (ADW_CLAMP (clamp), box);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), clamp);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scroll);
  adw_dialog_set_child (dialog, toolbar_view);

  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* ── Channel grid item callbacks ── */

static void
on_channel_card_clicked (GtkButton *button,
                         gpointer   user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  SatwaveChannel *channel = g_object_get_data (G_OBJECT (button), "channel");

  if (!channel)
    return;

  play_channel (self, channel);
}

static void
on_channel_info_clicked (GtkButton *button,
                         gpointer   user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  SatwaveChannel *channel = g_object_get_data (G_OBJECT (button), "channel");

  if (!channel)
    return;

  show_channel_detail (self, channel);
}

static void
on_stream_url_ready (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  g_autoptr (GError) error = NULL;

  g_autofree char *stream_url = satwave_api_client_get_stream_url_finish (
    self->api, result, &error);

  if (error) {
    g_warning ("Failed to get stream URL: %s", error->message);
    const char *user_msg = "This channel is not available for streaming";
    if (strstr (error->message, "not-found") || strstr (error->message, "not found"))
      user_msg = "This channel is not currently available for streaming";
    else if (strstr (error->message, "invalid-source"))
      user_msg = "This channel cannot be played";
    AdwToast *toast = adw_toast_new (user_msg);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
    return;
  }

  /* Set the upstream URL on the HLS proxy */
  satwave_hls_proxy_set_upstream (self->proxy, stream_url);

  /* Play via local proxy — it handles auth injection and AES key serving */
  const char *channel_id = satwave_channel_get_id (self->current_channel);
  g_autofree char *proxy_url = satwave_hls_proxy_get_url (self->proxy, channel_id);

  g_debug ("Playing via proxy: %s (upstream: %s)", proxy_url, stream_url);
  satwave_player_play_uri (self->player, proxy_url);

  /* Save last channel */
  g_settings_set_string (self->settings, "last-channel", channel_id);

  update_now_playing (self);

  /* Start polling for song/artist metadata from SXM API */
  start_now_playing_poll (self);
}

static void
on_favorite_toggled (GtkButton *button,
                     gpointer   user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  SatwaveChannel *channel = g_object_get_data (G_OBJECT (button), "channel");

  if (!channel)
    return;

  gboolean fav = !satwave_channel_get_is_favorite (channel);
  satwave_channel_set_is_favorite (channel, fav);

  /* Update icon */
  const char *icon = fav ? "starred-symbolic" : "non-starred-symbolic";
  gtk_button_set_icon_name (button, icon);

  /* Save to GSettings */
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));

  GListModel *model = satwave_channel_store_get_model (self->store);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++) {
    g_autoptr (SatwaveChannel) ch = g_list_model_get_item (model, i);
    if (satwave_channel_get_is_favorite (ch))
      g_variant_builder_add (&builder, "s", satwave_channel_get_id (ch));
  }

  g_settings_set_value (self->settings, "favorites", g_variant_builder_end (&builder));
}

static void
on_image_loaded (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GtkWidget *image = GTK_WIDGET (user_data);
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);

  if (error || !bytes) {
    g_object_unref (image);
    return;
  }

  gsize size;
  const guchar *data = g_bytes_get_data (bytes, &size);
  if (size == 0) {
    g_object_unref (image);
    return;
  }

  /* Create a GdkTexture from the downloaded image data */
  g_autoptr (GdkTexture) texture = gdk_texture_new_from_bytes (bytes, &error);
  if (texture && GTK_IS_IMAGE (image)) {
    gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
    gtk_image_set_pixel_size (GTK_IMAGE (image), 120);
  }

  g_object_unref (image);
}

static void
channel_grid_setup (GtkListItemFactory *factory,
                    GtkListItem        *list_item,
                    gpointer            user_data)
{
  /* Card container */
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (card, "card");
  gtk_widget_set_margin_start (card, 6);
  gtk_widget_set_margin_end (card, 6);
  gtk_widget_set_margin_top (card, 6);
  gtk_widget_set_margin_bottom (card, 6);

  /* Channel artwork placeholder */
  GtkWidget *image = gtk_image_new_from_icon_name ("audio-x-generic-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (image), 120);
  gtk_widget_set_margin_top (image, 12);
  gtk_widget_set_halign (image, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (card), image);

  /* Channel number + name */
  GtkWidget *number_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (number_label, "caption");
  gtk_widget_add_css_class (number_label, "dim-label");
  gtk_widget_set_halign (number_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (number_label, 12);
  gtk_box_append (GTK_BOX (card), number_label);

  GtkWidget *name_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (name_label, "heading");
  gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (name_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (name_label, 12);
  gtk_widget_set_margin_end (name_label, 12);
  gtk_box_append (GTK_BOX (card), name_label);

  GtkWidget *desc_label = gtk_label_new (NULL);
  gtk_widget_add_css_class (desc_label, "caption");
  gtk_widget_add_css_class (desc_label, "dim-label");
  gtk_label_set_ellipsize (GTK_LABEL (desc_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (desc_label), 30);
  gtk_widget_set_halign (desc_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (desc_label, 12);
  gtk_widget_set_margin_end (desc_label, 12);
  gtk_box_append (GTK_BOX (card), desc_label);

  /* Button row */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_end (btn_box, 8);
  gtk_widget_set_margin_bottom (btn_box, 8);
  gtk_widget_set_margin_top (btn_box, 4);

  GtkWidget *info_btn = gtk_button_new_from_icon_name ("help-about-symbolic");
  gtk_widget_add_css_class (info_btn, "flat");
  gtk_widget_add_css_class (info_btn, "circular");
  gtk_widget_set_tooltip_text (info_btn, "Channel details");
  gtk_box_append (GTK_BOX (btn_box), info_btn);

  GtkWidget *fav_btn = gtk_button_new_from_icon_name ("non-starred-symbolic");
  gtk_widget_add_css_class (fav_btn, "flat");
  gtk_widget_add_css_class (fav_btn, "circular");
  gtk_box_append (GTK_BOX (btn_box), fav_btn);

  GtkWidget *play_btn = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_add_css_class (play_btn, "suggested-action");
  gtk_widget_add_css_class (play_btn, "circular");
  gtk_box_append (GTK_BOX (btn_box), play_btn);

  gtk_box_append (GTK_BOX (card), btn_box);

  gtk_list_item_set_child (list_item, card);

  /* Store widget references for bind */
  g_object_set_data (G_OBJECT (list_item), "image", image);
  g_object_set_data (G_OBJECT (list_item), "number-label", number_label);
  g_object_set_data (G_OBJECT (list_item), "name-label", name_label);
  g_object_set_data (G_OBJECT (list_item), "desc-label", desc_label);
  g_object_set_data (G_OBJECT (list_item), "info-btn", info_btn);
  g_object_set_data (G_OBJECT (list_item), "fav-btn", fav_btn);
  g_object_set_data (G_OBJECT (list_item), "play-btn", play_btn);
}

static void
channel_grid_bind (GtkListItemFactory *factory,
                   GtkListItem        *list_item,
                   gpointer            user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  SatwaveChannel *channel = gtk_list_item_get_item (list_item);

  GtkWidget *number_label = g_object_get_data (G_OBJECT (list_item), "number-label");
  GtkWidget *name_label = g_object_get_data (G_OBJECT (list_item), "name-label");
  GtkWidget *desc_label = g_object_get_data (G_OBJECT (list_item), "desc-label");
  GtkWidget *fav_btn = g_object_get_data (G_OBJECT (list_item), "fav-btn");
  GtkWidget *play_btn = g_object_get_data (G_OBJECT (list_item), "play-btn");
  GtkWidget *image = g_object_get_data (G_OBJECT (list_item), "image");

  g_autofree char *num_text = g_strdup_printf ("CH %03d  •  %s",
    satwave_channel_get_number (channel),
    satwave_channel_get_category (channel));
  gtk_label_set_text (GTK_LABEL (number_label), num_text);
  gtk_label_set_text (GTK_LABEL (name_label), satwave_channel_get_name (channel));
  gtk_label_set_text (GTK_LABEL (desc_label), satwave_channel_get_description (channel));

  /* Favorite state */
  const char *fav_icon = satwave_channel_get_is_favorite (channel)
    ? "starred-symbolic" : "non-starred-symbolic";
  gtk_button_set_icon_name (GTK_BUTTON (fav_btn), fav_icon);

  /* Connect signals (store channel ref on buttons) */
  g_object_set_data_full (G_OBJECT (play_btn), "channel",
                          g_object_ref (channel), g_object_unref);
  g_signal_handlers_disconnect_by_func (play_btn, on_channel_card_clicked, self);
  g_signal_connect (play_btn, "clicked", G_CALLBACK (on_channel_card_clicked), self);

  g_object_set_data_full (G_OBJECT (fav_btn), "channel",
                          g_object_ref (channel), g_object_unref);
  g_signal_handlers_disconnect_by_func (fav_btn, on_favorite_toggled, self);
  g_signal_connect (fav_btn, "clicked", G_CALLBACK (on_favorite_toggled), self);

  GtkWidget *info_btn = g_object_get_data (G_OBJECT (list_item), "info-btn");
  g_object_set_data_full (G_OBJECT (info_btn), "channel",
                          g_object_ref (channel), g_object_unref);
  g_signal_handlers_disconnect_by_func (info_btn, on_channel_info_clicked, self);
  g_signal_connect (info_btn, "clicked", G_CALLBACK (on_channel_info_clicked), self);

  /* Load artwork asynchronously */
  const char *img_url = satwave_channel_get_image_url (channel);
  if (img_url && *img_url) {
    SoupMessage *img_msg = soup_message_new (SOUP_METHOD_GET, img_url);
    if (img_msg) {
      /* Store a weak ref to the image widget so we can update it when download completes */
      g_object_ref (image);
      soup_session_send_and_read_async (
        satwave_auth_get_session (self->auth), img_msg,
        G_PRIORITY_LOW, NULL,
        (GAsyncReadyCallback) on_image_loaded, image);
      g_object_unref (img_msg);
    }
  }
}

static void
on_np_image_loaded (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  GtkWidget *image = GTK_WIDGET (user_data);
  SoupSession *session = SOUP_SESSION (source);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (session, result, &error);
  if (!error && bytes) {
    g_autoptr (GdkTexture) texture = gdk_texture_new_from_bytes (bytes, NULL);
    if (texture && GTK_IS_IMAGE (image)) {
      gtk_image_set_from_paintable (GTK_IMAGE (image), GDK_PAINTABLE (texture));
      gtk_image_set_pixel_size (GTK_IMAGE (image), 48);
    }
  }

  g_object_unref (image);
}

/* ── Now playing bar ── */

static void
update_now_playing (SatwaveWindow *self)
{
  if (self->current_channel) {
    const char *ch_name = satwave_channel_get_name (self->current_channel);
    int ch_num = satwave_channel_get_number (self->current_channel);

    gtk_label_set_text (GTK_LABEL (self->np_channel_label), ch_name);
    gtk_widget_set_visible (self->now_playing_bar, TRUE);

    /* Update LCD top row: channel number */
    g_autofree char *freq_text = g_strdup_printf ("CH %03d", ch_num);
    gtk_label_set_text (GTK_LABEL (self->lcd_freq_label), freq_text);

    /* Load channel artwork into now-playing bar */
    const char *img_url = satwave_channel_get_image_url (self->current_channel);
    if (img_url && *img_url) {
      SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, img_url);
      if (msg) {
        g_object_ref (self->np_image);
        soup_session_send_and_read_async (
          satwave_auth_get_session (self->auth), msg,
          G_PRIORITY_DEFAULT, NULL,
          (GAsyncReadyCallback) on_np_image_loaded, self->np_image);
        g_object_unref (msg);
      }
    }
  }

  /* Build single LCD info line: "STATION  |  SONG - ARTIST" */
  const char *title = satwave_player_get_title (self->player);
  const char *artist = satwave_player_get_artist (self->player);
  const char *lcd_title_text = gtk_label_get_text (GTK_LABEL (self->lcd_title_label));

  /* Use API-fetched track info (lcd_title_label) if GStreamer tags are empty */
  const char *track_text = (title && *title) ? NULL : lcd_title_text;
  g_autofree char *gst_track = NULL;

  if (title && *title) {
    if (artist && *artist)
      gst_track = g_strdup_printf ("%s - %s", title, artist);
    else
      gst_track = g_strdup (title);
    track_text = gst_track;
    gtk_label_set_text (GTK_LABEL (self->np_title_label), gst_track);
  }

  if (self->current_channel) {
    const char *ch_name = satwave_channel_get_name (self->current_channel);
    g_autofree char *ch_upper = g_ascii_strup (ch_name, -1);

    if (track_text && *track_text) {
      g_autofree char *track_upper = g_ascii_strup (track_text, -1);
      g_autofree char *info_line = g_strdup_printf ("%s  \xC2\xB7  %s", ch_upper, track_upper);
      gtk_label_set_text (GTK_LABEL (self->lcd_channel_label), info_line);
    } else {
      gtk_label_set_text (GTK_LABEL (self->lcd_channel_label), ch_upper);
    }
  }

  /* Update play/pause button icon */
  const char *icon = satwave_player_is_playing (self->player)
    ? "media-playback-pause-symbolic"
    : "media-playback-start-symbolic";
  gtk_button_set_icon_name (GTK_BUTTON (self->play_button), icon);
}

/* ── Now-playing API polling ── */

static void
on_now_playing_fetched (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  g_autoptr (GError) error = NULL;

  SatwaveNowPlaying *np = satwave_api_client_get_now_playing_finish (self->api, result, &error);
  if (error) {
    g_debug ("Now-playing fetch failed: %s", error->message);
    return;
  }

  if (!np)
    return;

  /* Store track info and rebuild the single LCD info line */
  if (np->song_title && np->song_title[0]) {
    g_autofree char *lcd_track = NULL;
    if (np->artist_name && np->artist_name[0])
      lcd_track = g_strdup_printf ("%s - %s", np->song_title, np->artist_name);
    else
      lcd_track = g_strdup (np->song_title);

    g_autofree char *lcd_upper = g_ascii_strup (lcd_track, -1);
    gtk_label_set_text (GTK_LABEL (self->lcd_title_label), lcd_upper);
    gtk_label_set_text (GTK_LABEL (self->np_title_label), lcd_track);

    /* Rebuild combined info line */
    if (self->current_channel) {
      const char *ch_name = satwave_channel_get_name (self->current_channel);
      g_autofree char *ch_upper = g_ascii_strup (ch_name, -1);
      g_autofree char *info_line = g_strdup_printf ("%s  \xC2\xB7  %s", ch_upper, lcd_upper);
      gtk_label_set_text (GTK_LABEL (self->lcd_channel_label), info_line);
    }
  }

  satwave_now_playing_free (np);
}

static gboolean
poll_now_playing (gpointer user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (!self->current_channel || !satwave_player_is_playing (self->player))
    return G_SOURCE_CONTINUE;

  const char *channel_id = satwave_channel_get_id (self->current_channel);
  satwave_api_client_get_now_playing_async (self->api, channel_id, NULL,
                                            on_now_playing_fetched, self);

  return G_SOURCE_CONTINUE;
}

static void
start_now_playing_poll (SatwaveWindow *self)
{
  /* Stop existing poll if any */
  if (self->now_playing_poll_id > 0) {
    g_source_remove (self->now_playing_poll_id);
    self->now_playing_poll_id = 0;
  }

  /* Immediate first fetch */
  if (self->current_channel) {
    const char *channel_id = satwave_channel_get_id (self->current_channel);
    satwave_api_client_get_now_playing_async (self->api, channel_id, NULL,
                                              on_now_playing_fetched, self);
  }

  /* Poll every 15 seconds */
  self->now_playing_poll_id = g_timeout_add_seconds (10, poll_now_playing, self);
}

static void
on_metadata_changed (SatwavePlayer *player,
                     gpointer       user_data)
{
  update_now_playing (SATWAVE_WINDOW (user_data));
}

static void
on_player_state_changed (SatwavePlayer *player,
                         gpointer       user_data)
{
  update_now_playing (SATWAVE_WINDOW (user_data));
}

static void
on_player_buffering (SatwavePlayer *player,
                     int            percent,
                     gpointer       user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (percent < 100) {
    g_autofree char *msg = g_strdup_printf ("Buffering... %d%%", percent);
    gtk_label_set_text (GTK_LABEL (self->np_title_label), msg);
  }
}

static void
on_player_error (SatwavePlayer *player,
                 const char    *message,
                 gpointer       user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  AdwToast *toast = adw_toast_new (message);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

static void
on_np_art_clicked (GtkButton *button,
                   gpointer   user_data)
{
  (void)button;
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (self->current_channel)
    show_channel_detail (self, self->current_channel);
}

static void
on_play_pause_clicked (GtkButton *button,
                       gpointer   user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (satwave_player_is_playing (self->player))
    satwave_player_pause (self->player);
  else
    satwave_player_resume (self->player);
}

static void
on_volume_changed (GtkScaleButton *button,
                   double          value,
                   gpointer        user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  satwave_player_set_volume (self->player, value);
  g_settings_set_double (self->settings, "volume", value);
}

/* ── Search ── */

static void
on_search_changed (GtkSearchEntry *entry,
                   gpointer        user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  satwave_channel_store_set_search_filter (self->store, text);
}

/* ── Category list ── */

static void
on_category_selected (GtkListBox    *box,
                      GtkListBoxRow *row,
                      gpointer       user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (!row) {
    satwave_channel_store_set_category_filter (self->store, NULL);
    return;
  }

  int index = gtk_list_box_row_get_index (row);
  if (index == 0) {
    /* "All Channels" */
    satwave_channel_store_set_category_filter (self->store, NULL);
    satwave_channel_store_set_favorites_only (self->store, FALSE);
  } else if (index == 1) {
    /* "Favorites" */
    satwave_channel_store_set_category_filter (self->store, NULL);
    satwave_channel_store_set_favorites_only (self->store, TRUE);
  } else {
    /* Category name from the label */
    GtkWidget *child = gtk_list_box_row_get_child (row);
    if (GTK_IS_LABEL (child)) {
      const char *cat = gtk_label_get_text (GTK_LABEL (child));
      satwave_channel_store_set_favorites_only (self->store, FALSE);
      satwave_channel_store_set_category_filter (self->store, cat);
    }
  }
}

/* ── Auth flow ── */

static void
on_login_success (SatwaveLoginDialog *dialog,
                  gpointer            user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  load_channels (self);
}

static void
show_login_dialog (SatwaveWindow *self)
{
  SatwaveLoginDialog *dialog = satwave_login_dialog_new (self->auth);
  g_signal_connect (dialog, "login-success", G_CALLBACK (on_login_success), self);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_login_button_clicked (GtkButton *button,
                         gpointer   user_data)
{
  show_login_dialog (SATWAVE_WINDOW (user_data));
}

/* ── Channel loading ── */

static void
on_channels_loaded (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GListStore) channels =
    satwave_api_client_get_channels_finish (self->api, result, &error);

  if (error) {
    g_warning ("Failed to load channels: %s", error->message);
    gtk_stack_set_visible_child (GTK_STACK (self->main_stack), self->login_status_page);
    AdwToast *toast = adw_toast_new (error->message);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
    return;
  }

  satwave_channel_store_set_channels (self->store, channels);

  /* Restore favorites from GSettings */
  g_autoptr (GVariant) favs = g_settings_get_value (self->settings, "favorites");
  gsize n_favs;
  const char **fav_ids = g_variant_get_strv (favs, &n_favs);

  GListModel *model = satwave_channel_store_get_model (self->store);
  guint n_channels = g_list_model_get_n_items (model);
  for (guint i = 0; i < n_channels; i++) {
    g_autoptr (SatwaveChannel) ch = g_list_model_get_item (model, i);
    for (gsize j = 0; j < n_favs; j++) {
      if (g_strcmp0 (satwave_channel_get_id (ch), fav_ids[j]) == 0) {
        satwave_channel_set_is_favorite (ch, TRUE);
        break;
      }
    }
  }
  g_free (fav_ids);

  /* Populate category sidebar */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (self->category_list)))
    gtk_list_box_remove (GTK_LIST_BOX (self->category_list), child);

  /* "All Channels" row */
  GtkWidget *all_label = gtk_label_new ("All Channels");
  gtk_widget_set_halign (all_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (all_label, 8);
  gtk_widget_set_margin_end (all_label, 8);
  gtk_widget_set_margin_top (all_label, 4);
  gtk_widget_set_margin_bottom (all_label, 4);
  gtk_list_box_append (GTK_LIST_BOX (self->category_list), all_label);

  /* "Favorites" row */
  GtkWidget *fav_label = gtk_label_new ("Favorites");
  gtk_widget_set_halign (fav_label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (fav_label, 8);
  gtk_widget_set_margin_end (fav_label, 8);
  gtk_widget_set_margin_top (fav_label, 4);
  gtk_widget_set_margin_bottom (fav_label, 4);
  gtk_list_box_append (GTK_LIST_BOX (self->category_list), fav_label);

  /* Category rows */
  GListModel *categories = satwave_channel_store_get_categories (self->store);
  guint n_cats = g_list_model_get_n_items (categories);
  for (guint i = 0; i < n_cats; i++) {
    GtkStringObject *str_obj = g_list_model_get_item (categories, i);
    const char *cat_name = gtk_string_object_get_string (str_obj);

    GtkWidget *label = gtk_label_new (cat_name);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_widget_set_margin_start (label, 8);
    gtk_widget_set_margin_end (label, 8);
    gtk_widget_set_margin_top (label, 4);
    gtk_widget_set_margin_bottom (label, 4);
    gtk_list_box_append (GTK_LIST_BOX (self->category_list), label);

    g_object_unref (str_obj);
  }

  /* Show content */
  gtk_stack_set_visible_child (GTK_STACK (self->main_stack), self->content_box);
}

static void
load_channels (SatwaveWindow *self)
{
  gtk_stack_set_visible_child (GTK_STACK (self->main_stack), self->loading_status_page);
  satwave_api_client_get_channels_async (self->api, NULL, on_channels_loaded, self);
}

/* ── Auto-login on startup ── */

static void
on_restore_done (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);
  g_autoptr (GError) error = NULL;

  if (satwave_auth_restore_finish (self->auth, result, &error)) {
    g_debug ("Auto-login successful");
    load_channels (self);
  } else {
    g_debug ("Auto-login failed: %s", error->message);
    gtk_stack_set_visible_child (GTK_STACK (self->main_stack), self->login_status_page);
  }
}

/* ── Session refresh timer ── */

static gboolean
refresh_session (gpointer user_data)
{
  SatwaveWindow *self = SATWAVE_WINDOW (user_data);

  if (satwave_auth_is_authenticated (self->auth)) {
    g_debug ("Refreshing SiriusXM session...");
    satwave_auth_restore_async (self->auth, NULL, NULL, NULL);
  }

  return G_SOURCE_CONTINUE;
}

/* ── Build UI ── */

static GtkWidget *
build_now_playing_bar (SatwaveWindow *self)
{
  /* Use a GtkCenterBox: start=artwork, center=LCD, end=controls */
  GtkWidget *bar = gtk_center_box_new ();
  gtk_widget_add_css_class (bar, "toolbar");
  gtk_widget_set_margin_start (bar, 8);
  gtk_widget_set_margin_end (bar, 8);
  gtk_widget_set_margin_top (bar, 4);
  gtk_widget_set_margin_bottom (bar, 4);

  /* ── Start: Channel artwork ── */
  GtkWidget *start_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  self->np_image = gtk_image_new_from_icon_name ("audio-x-generic-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (self->np_image), 48);
  gtk_widget_add_css_class (self->np_image, "lcd-artwork");

  /* Wrap image in a button so clicking it opens channel detail */
  GtkWidget *np_image_btn = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (np_image_btn), self->np_image);
  gtk_widget_add_css_class (np_image_btn, "flat");
  gtk_widget_add_css_class (np_image_btn, "np-art-button");
  g_signal_connect (np_image_btn, "clicked",
                    G_CALLBACK (on_np_art_clicked), self);
  gtk_box_append (GTK_BOX (start_box), np_image_btn);

  /* Hidden labels — still updated for internal state */
  self->np_channel_label = gtk_label_new ("Not Playing");
  gtk_widget_set_visible (self->np_channel_label, FALSE);
  gtk_box_append (GTK_BOX (start_box), self->np_channel_label);
  self->np_title_label = gtk_label_new ("");
  gtk_widget_set_visible (self->np_title_label, FALSE);
  gtk_box_append (GTK_BOX (start_box), self->np_title_label);

  gtk_center_box_set_start_widget (GTK_CENTER_BOX (bar), start_box);

  /* ── Center: Retro LCD Display (70% width) ── */
  self->lcd_frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (self->lcd_frame, "lcd-display");
  gtk_widget_set_halign (self->lcd_frame, GTK_ALIGN_FILL);
  gtk_widget_set_valign (self->lcd_frame, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->lcd_frame, TRUE);

  GtkWidget *lcd_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class (lcd_box, "lcd-inner");
  gtk_widget_set_margin_start (lcd_box, 10);
  gtk_widget_set_margin_end (lcd_box, 10);
  gtk_widget_set_margin_top (lcd_box, 3);
  gtk_widget_set_margin_bottom (lcd_box, 3);
  gtk_widget_set_valign (lcd_box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand (lcd_box, TRUE);

  /* Top row: channel number */
  self->lcd_freq_label = gtk_label_new ("-- --- --");
  gtk_widget_add_css_class (self->lcd_freq_label, "lcd-freq");
  gtk_widget_set_halign (self->lcd_freq_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->lcd_freq_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (lcd_box), self->lcd_freq_label);

  /* Bottom row: "STATION  |  SONG - ARTIST" all on one line */
  self->lcd_channel_label = gtk_label_new ("SATWAVE");
  gtk_widget_add_css_class (self->lcd_channel_label, "lcd-info");
  gtk_label_set_wrap (GTK_LABEL (self->lcd_channel_label), FALSE);
  gtk_label_set_single_line_mode (GTK_LABEL (self->lcd_channel_label), TRUE);
  gtk_widget_set_halign (self->lcd_channel_label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->lcd_channel_label, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (self->lcd_channel_label, TRUE);
  gtk_box_append (GTK_BOX (lcd_box), self->lcd_channel_label);

  /* Hidden — we merge into lcd_channel_label but keep for state tracking */
  self->lcd_title_label = gtk_label_new ("");
  gtk_widget_set_visible (self->lcd_title_label, FALSE);
  gtk_box_append (GTK_BOX (lcd_box), self->lcd_title_label);

  gtk_box_append (GTK_BOX (self->lcd_frame), lcd_box);
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (bar), self->lcd_frame);

  /* ── End: Playback controls ── */
  GtkWidget *end_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  self->play_button = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
  gtk_widget_add_css_class (self->play_button, "circular");
  g_signal_connect (self->play_button, "clicked",
                    G_CALLBACK (on_play_pause_clicked), self);
  gtk_box_append (GTK_BOX (end_box), self->play_button);

  const char *vol_icons[] = {
    "audio-volume-muted-symbolic",
    "audio-volume-low-symbolic",
    "audio-volume-medium-symbolic",
    "audio-volume-high-symbolic",
    NULL
  };
  self->volume_button = gtk_scale_button_new (0.0, 1.0, 0.05, vol_icons);
  double vol = g_settings_get_double (self->settings, "volume");
  gtk_scale_button_set_value (GTK_SCALE_BUTTON (self->volume_button), vol);
  g_signal_connect (self->volume_button, "value-changed",
                    G_CALLBACK (on_volume_changed), self);
  gtk_box_append (GTK_BOX (end_box), self->volume_button);

  gtk_center_box_set_end_widget (GTK_CENTER_BOX (bar), end_box);

  return bar;
}

static void
satwave_window_dispose (GObject *object)
{
  SatwaveWindow *self = SATWAVE_WINDOW (object);

  if (self->session_refresh_id > 0) {
    g_source_remove (self->session_refresh_id);
    self->session_refresh_id = 0;
  }

  if (self->now_playing_poll_id > 0) {
    g_source_remove (self->now_playing_poll_id);
    self->now_playing_poll_id = 0;
  }

  satwave_player_stop (self->player);
  satwave_hls_proxy_stop (self->proxy);

  /* Save window state */
  int width, height;
  gtk_window_get_default_size (GTK_WINDOW (self), &width, &height);
  g_settings_set_int (self->settings, "window-width", width);
  g_settings_set_int (self->settings, "window-height", height);
  g_settings_set_boolean (self->settings, "window-maximized",
                          gtk_window_is_maximized (GTK_WINDOW (self)));

  g_clear_object (&self->auth);
  g_clear_object (&self->api);
  g_clear_object (&self->player);
  g_clear_object (&self->proxy);
  g_clear_object (&self->store);
  g_clear_object (&self->settings);
  g_clear_object (&self->current_channel);

  G_OBJECT_CLASS (satwave_window_parent_class)->dispose (object);
}

static void
satwave_window_class_init (SatwaveWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_window_dispose;
}

static void
satwave_window_init (SatwaveWindow *self)
{
  /* Core objects */
  self->settings = g_settings_new (APP_ID);
  self->auth = satwave_auth_new ();
  self->api = satwave_api_client_new (self->auth);
  self->player = satwave_player_new ();
  self->proxy = satwave_hls_proxy_new (self->auth);
  self->store = satwave_channel_store_new ();

  /* Connect player signals */
  g_signal_connect (self->player, "metadata-changed",
                    G_CALLBACK (on_metadata_changed), self);
  g_signal_connect (self->player, "state-changed",
                    G_CALLBACK (on_player_state_changed), self);
  g_signal_connect (self->player, "buffering",
                    G_CALLBACK (on_player_buffering), self);
  g_signal_connect (self->player, "error",
                    G_CALLBACK (on_player_error), self);

  /* Set volume from settings */
  double vol = g_settings_get_double (self->settings, "volume");
  satwave_player_set_volume (self->player, vol);

  /* Start HLS proxy */
  g_autoptr (GError) proxy_error = NULL;
  if (!satwave_hls_proxy_start (self->proxy, &proxy_error))
    g_warning ("Failed to start HLS proxy: %s", proxy_error->message);

  /* ── Build window layout ── */

  /* Restore window size */
  int width = g_settings_get_int (self->settings, "window-width");
  int height = g_settings_get_int (self->settings, "window-height");
  gtk_window_set_default_size (GTK_WINDOW (self), width, height);
  if (g_settings_get_boolean (self->settings, "window-maximized"))
    gtk_window_maximize (GTK_WINDOW (self));

  gtk_window_set_title (GTK_WINDOW (self), "Satwave");
  gtk_window_set_icon_name (GTK_WINDOW (self), APP_ID);

  /* Toast overlay wraps everything */
  self->toast_overlay = adw_toast_overlay_new ();

  /* AdwToolbarView: header + content + bottom bar */
  GtkWidget *toolbar_view = adw_toolbar_view_new ();

  /* Header bar */
  self->header_bar = adw_header_bar_new ();

  /* Search toggle in header */
  self->search_button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (self->search_button), "system-search-symbolic");
  adw_header_bar_pack_end (ADW_HEADER_BAR (self->header_bar),
                           self->search_button);

  /* Menu button */
  GMenu *menu = g_menu_new ();
  g_menu_append (menu, "About Satwave", "app.about");
  g_menu_append (menu, "Quit", "app.quit");
  GtkWidget *menu_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), G_MENU_MODEL (menu));
  adw_header_bar_pack_end (ADW_HEADER_BAR (self->header_bar), menu_button);
  g_object_unref (menu);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), self->header_bar);

  /* Search bar below header */
  self->search_bar = gtk_search_bar_new ();
  self->search_entry = gtk_search_entry_new ();
  gtk_search_bar_set_child (GTK_SEARCH_BAR (self->search_bar), self->search_entry);
  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar),
                                GTK_EDITABLE (self->search_entry));
  g_object_bind_property (self->search_button, "active",
                          self->search_bar, "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_signal_connect (self->search_entry, "search-changed",
                    G_CALLBACK (on_search_changed), self);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), self->search_bar);

  /* ── Main content area ── */
  self->main_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->main_stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  /* Login status page */
  self->login_status_page = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->login_status_page),
                                 "dialog-password-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->login_status_page),
                             "Sign In to SiriusXM");
  adw_status_page_set_description (ADW_STATUS_PAGE (self->login_status_page),
                                   "Sign in with your SiriusXM account to start listening");

  GtkWidget *login_btn = gtk_button_new_with_label ("Sign In");
  gtk_widget_add_css_class (login_btn, "suggested-action");
  gtk_widget_add_css_class (login_btn, "pill");
  gtk_widget_set_halign (login_btn, GTK_ALIGN_CENTER);
  g_signal_connect (login_btn, "clicked", G_CALLBACK (on_login_button_clicked), self);
  adw_status_page_set_child (ADW_STATUS_PAGE (self->login_status_page), login_btn);

  gtk_stack_add_named (GTK_STACK (self->main_stack), self->login_status_page, "login");

  /* Loading status page */
  self->loading_status_page = adw_status_page_new ();
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 48, 48);
  adw_status_page_set_child (ADW_STATUS_PAGE (self->loading_status_page), spinner);
  adw_status_page_set_title (ADW_STATUS_PAGE (self->loading_status_page),
                             "Loading Channels...");

  gtk_stack_add_named (GTK_STACK (self->main_stack), self->loading_status_page, "loading");

  /* Content: split view with category sidebar + channel grid */
  self->content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  /* Category sidebar */
  GtkWidget *sidebar_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (sidebar_scroll, 200, -1);

  self->category_list = gtk_list_box_new ();
  gtk_widget_add_css_class (self->category_list, "navigation-sidebar");
  g_signal_connect (self->category_list, "row-selected",
                    G_CALLBACK (on_category_selected), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                 self->category_list);

  gtk_box_append (GTK_BOX (self->content_box), sidebar_scroll);

  /* Separator */
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
  gtk_box_append (GTK_BOX (self->content_box), sep);

  /* Channel grid */
  self->channel_scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_hexpand (self->channel_scrolled, TRUE);
  gtk_widget_set_vexpand (self->channel_scrolled, TRUE);

  /* Set up GtkGridView with factory */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (channel_grid_setup), self);
  g_signal_connect (factory, "bind", G_CALLBACK (channel_grid_bind), self);

  GListModel *filtered = satwave_channel_store_get_filtered (self->store);
  GtkNoSelection *selection = gtk_no_selection_new (g_object_ref (filtered));

  self->channel_grid = gtk_grid_view_new (GTK_SELECTION_MODEL (selection), factory);
  gtk_grid_view_set_min_columns (GTK_GRID_VIEW (self->channel_grid), 1);
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (self->channel_grid), 5);
  gtk_widget_add_css_class (self->channel_grid, "content-grid");

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->channel_scrolled),
                                 self->channel_grid);
  gtk_box_append (GTK_BOX (self->content_box), self->channel_scrolled);

  gtk_stack_add_named (GTK_STACK (self->main_stack), self->content_box, "content");

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), self->main_stack);

  /* Now playing bar at bottom */
  self->now_playing_bar = build_now_playing_bar (self);
  gtk_widget_set_visible (self->now_playing_bar, FALSE);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), self->now_playing_bar);

  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (self->toast_overlay), toolbar_view);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), self->toast_overlay);

  /* Start session refresh timer (every 3.5 hours = 12600s, before 4h expiry) */
  self->session_refresh_id = g_timeout_add_seconds (12600, refresh_session, self);

  /* Try auto-login */
  gtk_stack_set_visible_child (GTK_STACK (self->main_stack), self->loading_status_page);
  satwave_auth_restore_async (self->auth, NULL, on_restore_done, self);
}

SatwaveWindow *
satwave_window_new (SatwaveApplication *app)
{
  return g_object_new (SATWAVE_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
