#pragma once

#include <adwaita.h>
#include "satwave-application.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_WINDOW (satwave_window_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveWindow, satwave_window, SATWAVE, WINDOW, AdwApplicationWindow)

SatwaveWindow *satwave_window_new (SatwaveApplication *app);

void satwave_window_play_channel_by_id (SatwaveWindow *self,
                                        const char    *channel_id);

G_END_DECLS
