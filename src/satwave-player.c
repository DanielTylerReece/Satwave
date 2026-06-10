#include "satwave-player.h"
#include <gst/gst.h>

struct _SatwavePlayer {
  GObject     parent_instance;

  GstElement *playbin;
  GstBus     *bus;
  guint       bus_watch_id;
  guint       retry_id;
  double      volume;
  gboolean    is_playing;
  char       *title;
  char       *artist;
};

enum {
  SIGNAL_METADATA_CHANGED,
  SIGNAL_STATE_CHANGED,
  SIGNAL_BUFFERING,
  SIGNAL_ERROR,
  SIGNAL_EOS,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (SatwavePlayer, satwave_player, G_TYPE_OBJECT)

static gboolean
retry_play (gpointer user_data)
{
  SatwavePlayer *self = SATWAVE_PLAYER (user_data);
  self->retry_id = 0;
  g_debug ("Retrying playback...");
  gst_element_set_state (self->playbin, GST_STATE_PLAYING);
  return G_SOURCE_REMOVE;
}

static void
cancel_retry (SatwavePlayer *self)
{
  if (self->retry_id > 0) {
    g_source_remove (self->retry_id);
    self->retry_id = 0;
  }
}

static gboolean
on_bus_message (GstBus     *bus,
                GstMessage *msg,
                gpointer    user_data)
{
  SatwavePlayer *self = SATWAVE_PLAYER (user_data);

  switch (GST_MESSAGE_TYPE (msg)) {
  case GST_MESSAGE_TAG: {
    GstTagList *tags = NULL;
    gst_message_parse_tag (msg, &tags);

    gchar *title = NULL, *artist = NULL;
    gst_tag_list_get_string (tags, GST_TAG_TITLE, &title);
    gst_tag_list_get_string (tags, GST_TAG_ARTIST, &artist);

    gboolean changed = FALSE;
    if (title) {
      if (g_strcmp0 (self->title, title) != 0) {
        g_free (self->title);
        self->title = g_strdup (title);
        changed = TRUE;
      }
      g_free (title);
    }
    if (artist) {
      if (g_strcmp0 (self->artist, artist) != 0) {
        g_free (self->artist);
        self->artist = g_strdup (artist);
        changed = TRUE;
      }
      g_free (artist);
    }

    if (changed)
      g_signal_emit (self, signals[SIGNAL_METADATA_CHANGED], 0);

    gst_tag_list_unref (tags);
    break;
  }

  case GST_MESSAGE_BUFFERING: {
    gint percent = 0;
    gst_message_parse_buffering (msg, &percent);

    if (percent < 100)
      gst_element_set_state (self->playbin, GST_STATE_PAUSED);
    else if (self->is_playing)
      gst_element_set_state (self->playbin, GST_STATE_PLAYING);

    g_signal_emit (self, signals[SIGNAL_BUFFERING], 0, percent);
    break;
  }

  case GST_MESSAGE_ERROR: {
    GError *err = NULL;
    gchar *debug = NULL;
    gst_message_parse_error (msg, &err, &debug);
    g_warning ("GStreamer error: %s (%s)", err->message, debug ? debug : "");

    g_signal_emit (self, signals[SIGNAL_ERROR], 0, err->message);

    gst_element_set_state (self->playbin, GST_STATE_READY);
    if (self->retry_id == 0)
      self->retry_id = g_timeout_add_seconds (3, retry_play, self);

    g_error_free (err);
    g_free (debug);
    break;
  }

  case GST_MESSAGE_STATE_CHANGED: {
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (self->playbin))
      g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
    break;
  }

  case GST_MESSAGE_EOS:
    g_debug ("End of stream");
    gst_element_set_state (self->playbin, GST_STATE_READY);
    self->is_playing = FALSE;
    g_signal_emit (self, signals[SIGNAL_EOS], 0);
    g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
    break;

  default:
    break;
  }

  return G_SOURCE_CONTINUE;
}

static void
satwave_player_dispose (GObject *object)
{
  SatwavePlayer *self = SATWAVE_PLAYER (object);

  cancel_retry (self);

  if (self->bus_watch_id > 0) {
    g_source_remove (self->bus_watch_id);
    self->bus_watch_id = 0;
  }

  if (self->playbin) {
    gst_element_set_state (self->playbin, GST_STATE_NULL);
    g_clear_pointer (&self->bus, gst_object_unref);
    g_clear_pointer (&self->playbin, gst_object_unref);
  }

  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->artist, g_free);

  G_OBJECT_CLASS (satwave_player_parent_class)->dispose (object);
}

static void
satwave_player_class_init (SatwavePlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_player_dispose;

  signals[SIGNAL_METADATA_CHANGED] =
    g_signal_new ("metadata-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_BUFFERING] =
    g_signal_new ("buffering",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_INT);

  signals[SIGNAL_ERROR] =
    g_signal_new ("error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[SIGNAL_EOS] =
    g_signal_new ("eos",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
satwave_player_init (SatwavePlayer *self)
{
  self->playbin = gst_element_factory_make ("playbin3", "satwave-playbin");
  if (!self->playbin)
    self->playbin = gst_element_factory_make ("playbin", "satwave-playbin");
  g_assert (self->playbin != NULL);

  self->volume = 0.8;
  g_object_set (self->playbin, "volume", self->volume, NULL);

  self->bus = gst_element_get_bus (self->playbin);
  self->bus_watch_id = gst_bus_add_watch (self->bus, on_bus_message, self);
}

SatwavePlayer *
satwave_player_new (void)
{
  return g_object_new (SATWAVE_TYPE_PLAYER, NULL);
}

void
satwave_player_play_uri (SatwavePlayer *self,
                         const char    *uri)
{
  cancel_retry (self);

  /* New stream — old tags no longer apply */
  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->artist, g_free);

  gst_element_set_state (self->playbin, GST_STATE_READY);
  g_object_set (self->playbin, "uri", uri, NULL);
  gst_element_set_state (self->playbin, GST_STATE_PLAYING);
  self->is_playing = TRUE;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

void
satwave_player_pause (SatwavePlayer *self)
{
  cancel_retry (self);
  gst_element_set_state (self->playbin, GST_STATE_PAUSED);
  self->is_playing = FALSE;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

void
satwave_player_resume (SatwavePlayer *self)
{
  gst_element_set_state (self->playbin, GST_STATE_PLAYING);
  self->is_playing = TRUE;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

void
satwave_player_stop (SatwavePlayer *self)
{
  cancel_retry (self);
  gst_element_set_state (self->playbin, GST_STATE_NULL);
  self->is_playing = FALSE;

  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->artist, g_free);

  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean
satwave_player_is_playing (SatwavePlayer *self)
{
  return self->is_playing;
}

void
satwave_player_set_volume (SatwavePlayer *self,
                           double         volume)
{
  self->volume = CLAMP (volume, 0.0, 1.0);
  g_object_set (self->playbin, "volume", self->volume, NULL);
}

double
satwave_player_get_volume (SatwavePlayer *self)
{
  return self->volume;
}

void
satwave_player_clear_metadata (SatwavePlayer *self)
{
  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->artist, g_free);
}

const char *
satwave_player_get_title (SatwavePlayer *self)
{
  return self->title;
}

const char *
satwave_player_get_artist (SatwavePlayer *self)
{
  return self->artist;
}
