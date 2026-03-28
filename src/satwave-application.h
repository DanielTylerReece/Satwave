#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define SATWAVE_TYPE_APPLICATION (satwave_application_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveApplication, satwave_application, SATWAVE, APPLICATION, AdwApplication)

SatwaveApplication *satwave_application_new (void);

G_END_DECLS
