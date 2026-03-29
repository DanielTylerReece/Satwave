#pragma once

#include <glib-object.h>
#include "satwave-auth.h"
#include "satwave-channel.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_API_CLIENT (satwave_api_client_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveApiClient, satwave_api_client, SATWAVE, API_CLIENT, GObject)

SatwaveApiClient *satwave_api_client_new (SatwaveAuth *auth);

/* Fetch the complete channel listing */
void         satwave_api_client_get_channels_async  (SatwaveApiClient    *self,
                                                     GCancellable        *cancellable,
                                                     GAsyncReadyCallback  callback,
                                                     gpointer             user_data);
GListStore  *satwave_api_client_get_channels_finish (SatwaveApiClient  *self,
                                                     GAsyncResult      *result,
                                                     GError           **error);

/* Stream info result — may contain multiple URLs for xtra channels */
typedef struct {
  char **urls;              /* NULL-terminated array of stream URLs */
  int    n_urls;
  char  *sequence_token;    /* Non-NULL for xtra channels; pass back for next batch */
  char  *source_context_id; /* xtra channel session context; needed for peek */
} SatwaveStreamInfo;

void                 satwave_stream_info_free (SatwaveStreamInfo *info);

/* Get the HLS stream URL for a channel */
void                 satwave_api_client_get_stream_url_async  (SatwaveApiClient    *self,
                                                               SatwaveChannel      *channel,
                                                               const char          *sequence_token,
                                                               GCancellable        *cancellable,
                                                               GAsyncReadyCallback  callback,
                                                               gpointer             user_data);
SatwaveStreamInfo   *satwave_api_client_get_stream_url_finish (SatwaveApiClient  *self,
                                                               GAsyncResult      *result,
                                                               GError           **error);

/* Peek for next batch of xtra channel tracks */
void                 satwave_api_client_peek_async  (SatwaveApiClient    *self,
                                                     SatwaveChannel      *channel,
                                                     const char          *sequence_token,
                                                     const char          *source_context_id,
                                                     GCancellable        *cancellable,
                                                     GAsyncReadyCallback  callback,
                                                     gpointer             user_data);
SatwaveStreamInfo   *satwave_api_client_peek_finish (SatwaveApiClient  *self,
                                                     GAsyncResult      *result,
                                                     GError           **error);

/* Now-playing metadata result */
typedef struct {
  char *song_title;
  char *artist_name;
  char *show_name;
} SatwaveNowPlaying;

void                 satwave_now_playing_free (SatwaveNowPlaying *np);

/* Fetch now-playing info for a channel */
void                 satwave_api_client_get_now_playing_async  (SatwaveApiClient    *self,
                                                                const char          *channel_id,
                                                                GCancellable        *cancellable,
                                                                GAsyncReadyCallback  callback,
                                                                gpointer             user_data);
SatwaveNowPlaying   *satwave_api_client_get_now_playing_finish (SatwaveApiClient  *self,
                                                                GAsyncResult      *result,
                                                                GError           **error);

/* Fetch long channel description from siriusxm.com */
void         satwave_api_client_get_channel_desc_async  (SatwaveApiClient    *self,
                                                         const char          *channel_name,
                                                         GCancellable        *cancellable,
                                                         GAsyncReadyCallback  callback,
                                                         gpointer             user_data);
char        *satwave_api_client_get_channel_desc_finish (SatwaveApiClient  *self,
                                                         GAsyncResult      *result,
                                                         GError           **error);

G_END_DECLS
