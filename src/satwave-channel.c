#include "satwave-channel.h"

struct _SatwaveChannel {
  GObject  parent_instance;

  char    *id;
  char    *guid;
  char    *name;
  int      number;
  char    *description;
  char    *category;
  char    *image_url;
  char    *entity_type;
  gboolean is_favorite;
};

enum {
  PROP_0,
  PROP_ID,
  PROP_GUID,
  PROP_NAME,
  PROP_NUMBER,
  PROP_DESCRIPTION,
  PROP_CATEGORY,
  PROP_IMAGE_URL,
  PROP_IS_FAVORITE,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_TYPE (SatwaveChannel, satwave_channel, G_TYPE_OBJECT)

static void
satwave_channel_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  SatwaveChannel *self = SATWAVE_CHANNEL (object);

  switch (prop_id) {
  case PROP_ID:
    g_value_set_string (value, self->id);
    break;
  case PROP_GUID:
    g_value_set_string (value, self->guid);
    break;
  case PROP_NAME:
    g_value_set_string (value, self->name);
    break;
  case PROP_NUMBER:
    g_value_set_int (value, self->number);
    break;
  case PROP_DESCRIPTION:
    g_value_set_string (value, self->description);
    break;
  case PROP_CATEGORY:
    g_value_set_string (value, self->category);
    break;
  case PROP_IMAGE_URL:
    g_value_set_string (value, self->image_url);
    break;
  case PROP_IS_FAVORITE:
    g_value_set_boolean (value, self->is_favorite);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
satwave_channel_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  SatwaveChannel *self = SATWAVE_CHANNEL (object);

  switch (prop_id) {
  case PROP_IS_FAVORITE:
    self->is_favorite = g_value_get_boolean (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
satwave_channel_finalize (GObject *object)
{
  SatwaveChannel *self = SATWAVE_CHANNEL (object);

  g_free (self->id);
  g_free (self->guid);
  g_free (self->name);
  g_free (self->description);
  g_free (self->category);
  g_free (self->image_url);
  g_free (self->entity_type);

  G_OBJECT_CLASS (satwave_channel_parent_class)->finalize (object);
}

static void
satwave_channel_class_init (SatwaveChannelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = satwave_channel_get_property;
  object_class->set_property = satwave_channel_set_property;
  object_class->finalize = satwave_channel_finalize;

  properties[PROP_ID] =
    g_param_spec_string ("id", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_GUID] =
    g_param_spec_string ("guid", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_NUMBER] =
    g_param_spec_int ("number", NULL, NULL, 0, 9999, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_DESCRIPTION] =
    g_param_spec_string ("description", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_CATEGORY] =
    g_param_spec_string ("category", NULL, NULL, "", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IMAGE_URL] =
    g_param_spec_string ("image-url", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_IS_FAVORITE] =
    g_param_spec_boolean ("is-favorite", NULL, NULL, FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
satwave_channel_init (SatwaveChannel *self)
{
}

SatwaveChannel *
satwave_channel_new (const char *id,
                     const char *guid,
                     const char *name,
                     int         number,
                     const char *description,
                     const char *category,
                     const char *image_url,
                     const char *entity_type,
                     gboolean    is_favorite)
{
  SatwaveChannel *self = g_object_new (SATWAVE_TYPE_CHANNEL, NULL);
  self->id = g_strdup (id);
  self->guid = g_strdup (guid);
  self->name = g_strdup (name);
  self->number = number;
  self->description = g_strdup (description);
  self->category = g_strdup (category);
  self->entity_type = g_strdup (entity_type ? entity_type : "channel-linear");
  self->image_url = g_strdup (image_url);
  self->is_favorite = is_favorite;
  return self;
}

const char *
satwave_channel_get_id (SatwaveChannel *self) { return self->id; }

const char *
satwave_channel_get_guid (SatwaveChannel *self) { return self->guid; }

const char *
satwave_channel_get_name (SatwaveChannel *self) { return self->name; }

int
satwave_channel_get_number (SatwaveChannel *self) { return self->number; }

const char *
satwave_channel_get_description (SatwaveChannel *self) { return self->description; }

const char *
satwave_channel_get_category (SatwaveChannel *self) { return self->category; }

const char *
satwave_channel_get_image_url (SatwaveChannel *self) { return self->image_url; }

const char *
satwave_channel_get_entity_type (SatwaveChannel *self) { return self->entity_type; }

gboolean
satwave_channel_get_is_favorite (SatwaveChannel *self) { return self->is_favorite; }

void
satwave_channel_set_is_favorite (SatwaveChannel *self,
                                 gboolean        is_favorite)
{
  if (self->is_favorite != is_favorite) {
    self->is_favorite = is_favorite;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IS_FAVORITE]);
  }
}
