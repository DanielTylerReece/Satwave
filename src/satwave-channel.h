#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SATWAVE_TYPE_CHANNEL (satwave_channel_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveChannel, satwave_channel, SATWAVE, CHANNEL, GObject)

SatwaveChannel *satwave_channel_new (const char *id,
                                     const char *guid,
                                     const char *name,
                                     int         number,
                                     const char *description,
                                     const char *category,
                                     const char *image_url,
                                     gboolean    is_favorite);

const char     *satwave_channel_get_id          (SatwaveChannel *self);
const char     *satwave_channel_get_guid        (SatwaveChannel *self);
const char     *satwave_channel_get_name        (SatwaveChannel *self);
int             satwave_channel_get_number      (SatwaveChannel *self);
const char     *satwave_channel_get_description (SatwaveChannel *self);
const char     *satwave_channel_get_category    (SatwaveChannel *self);
const char     *satwave_channel_get_image_url   (SatwaveChannel *self);

gboolean        satwave_channel_get_is_favorite (SatwaveChannel *self);
void            satwave_channel_set_is_favorite (SatwaveChannel *self,
                                                 gboolean        is_favorite);

G_END_DECLS
