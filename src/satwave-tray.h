#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SATWAVE_TYPE_TRAY (satwave_tray_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveTray, satwave_tray, SATWAVE, TRAY, GObject)

SatwaveTray *satwave_tray_new (void);

G_END_DECLS
