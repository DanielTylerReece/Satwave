#pragma once

#include <glib-object.h>
#include "satwave-player.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_MPRIS (satwave_mpris_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveMpris, satwave_mpris, SATWAVE, MPRIS, GObject)

SatwaveMpris *satwave_mpris_new (SatwavePlayer *player);

void satwave_mpris_set_metadata (SatwaveMpris *self,
                                 const char   *title,
                                 const char   *artist,
                                 const char   *channel_name,
                                 const char   *art_url);
void satwave_mpris_set_playback_status (SatwaveMpris *self,
                                        gboolean      playing);

G_END_DECLS
