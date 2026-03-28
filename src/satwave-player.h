#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SATWAVE_TYPE_PLAYER (satwave_player_get_type ())

G_DECLARE_FINAL_TYPE (SatwavePlayer, satwave_player, SATWAVE, PLAYER, GObject)

SatwavePlayer *satwave_player_new        (void);

void           satwave_player_play_uri   (SatwavePlayer *self,
                                          const char    *uri);
void           satwave_player_pause      (SatwavePlayer *self);
void           satwave_player_resume     (SatwavePlayer *self);
void           satwave_player_stop       (SatwavePlayer *self);
gboolean       satwave_player_is_playing (SatwavePlayer *self);

void           satwave_player_set_volume (SatwavePlayer *self,
                                          double         volume);
double         satwave_player_get_volume (SatwavePlayer *self);

const char    *satwave_player_get_title  (SatwavePlayer *self);
const char    *satwave_player_get_artist (SatwavePlayer *self);

G_END_DECLS
