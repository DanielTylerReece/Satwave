#pragma once

#include <gtk/gtk.h>
#include "satwave-channel.h"

G_BEGIN_DECLS

#define SATWAVE_TYPE_CHANNEL_STORE (satwave_channel_store_get_type ())

G_DECLARE_FINAL_TYPE (SatwaveChannelStore, satwave_channel_store, SATWAVE, CHANNEL_STORE, GObject)

SatwaveChannelStore *satwave_channel_store_new (void);

/* Populate from API response */
void         satwave_channel_store_set_channels   (SatwaveChannelStore *self,
                                                    GListStore          *channels);
GListModel  *satwave_channel_store_get_model      (SatwaveChannelStore *self);
GListModel  *satwave_channel_store_get_filtered   (SatwaveChannelStore *self);

/* Filter by category */
void         satwave_channel_store_set_category_filter (SatwaveChannelStore *self,
                                                        const char          *category);
/* Filter by search text */
void         satwave_channel_store_set_search_filter   (SatwaveChannelStore *self,
                                                        const char          *search);
/* Filter favorites only */
void         satwave_channel_store_set_favorites_only  (SatwaveChannelStore *self,
                                                        gboolean             favorites_only);

/* Get unique category list */
GListModel  *satwave_channel_store_get_categories (SatwaveChannelStore *self);

G_END_DECLS
