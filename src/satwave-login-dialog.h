#pragma once

#include <adwaita.h>
#include "satwave-auth.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_LOGIN_DIALOG (satwave_login_dialog_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveLoginDialog, satwave_login_dialog, SATWAVE, LOGIN_DIALOG, AdwDialog)

SatwaveLoginDialog *satwave_login_dialog_new (SatwaveAuth *auth);

G_END_DECLS
