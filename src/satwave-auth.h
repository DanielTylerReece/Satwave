#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define SATWAVE_TYPE_AUTH (satwave_auth_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveAuth, satwave_auth, SATWAVE, AUTH, GObject)

SatwaveAuth *satwave_auth_new              (void);

void         satwave_auth_login_async      (SatwaveAuth         *self,
                                            const char          *username,
                                            const char          *password,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean     satwave_auth_login_finish     (SatwaveAuth   *self,
                                            GAsyncResult  *result,
                                            GError       **error);

void         satwave_auth_restore_async    (SatwaveAuth         *self,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean     satwave_auth_restore_finish   (SatwaveAuth   *self,
                                            GAsyncResult  *result,
                                            GError       **error);

void         satwave_auth_logout           (SatwaveAuth *self);

gboolean     satwave_auth_is_authenticated (SatwaveAuth *self);

/* JWT access token for Bearer auth */
const char  *satwave_auth_get_access_token (SatwaveAuth *self);

/* Get the SoupSession (shared, no cookies needed — JWT based) */
SoupSession *satwave_auth_get_session      (SatwaveAuth *self);

/* Store credentials in GNOME Keyring after successful login */
void         satwave_auth_save_credentials (SatwaveAuth *self,
                                            const char  *username,
                                            const char  *password);

G_END_DECLS
