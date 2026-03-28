#pragma once

#include <glib-object.h>
#include "satwave-auth.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_HLS_PROXY (satwave_hls_proxy_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveHlsProxy, satwave_hls_proxy, SATWAVE, HLS_PROXY, GObject)

SatwaveHlsProxy *satwave_hls_proxy_new      (SatwaveAuth *auth);

gboolean          satwave_hls_proxy_start    (SatwaveHlsProxy  *self,
                                              GError          **error);
void              satwave_hls_proxy_stop     (SatwaveHlsProxy  *self);

/* Get the local proxy URL for a given CDN HLS URL */
char             *satwave_hls_proxy_get_url  (SatwaveHlsProxy *self,
                                              const char      *channel_id);
guint16           satwave_hls_proxy_get_port (SatwaveHlsProxy *self);

/* Set the current upstream HLS URL to proxy */
void              satwave_hls_proxy_set_upstream (SatwaveHlsProxy *self,
                                                  const char      *upstream_url);

G_END_DECLS
