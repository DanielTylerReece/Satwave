#include "satwave-channel-store.h"

struct _SatwaveChannelStore {
  GObject        parent_instance;

  GListStore    *channels;       /* Full channel list */
  GtkFilterListModel *filtered;  /* Filtered view */
  GtkEveryFilter *filter;        /* Combined filter */

  GtkCustomFilter *search_filter;
  GtkCustomFilter *category_filter;
  GtkCustomFilter *favorites_filter;

  char           *category;
  char           *search_text;
  char           *search_folded;  /* lowercased once per query, not per item */
  gboolean        favorites_only;

  GtkStringList  *categories;
};

G_DEFINE_TYPE (SatwaveChannelStore, satwave_channel_store, G_TYPE_OBJECT)

static gboolean
category_filter_func (gpointer item,
                      gpointer user_data)
{
  SatwaveChannelStore *self = SATWAVE_CHANNEL_STORE (user_data);
  SatwaveChannel *channel = SATWAVE_CHANNEL (item);

  if (!self->category || self->category[0] == '\0')
    return TRUE;

  return g_strcmp0 (satwave_channel_get_category (channel), self->category) == 0;
}

static gboolean
search_filter_func (gpointer item,
                    gpointer user_data)
{
  SatwaveChannelStore *self = SATWAVE_CHANNEL_STORE (user_data);
  SatwaveChannel *channel = SATWAVE_CHANNEL (item);

  if (!self->search_text || self->search_text[0] == '\0')
    return TRUE;

  const char *search_lower = self->search_folded;

  /* Match against channel name */
  const char *name = satwave_channel_get_name (channel);
  if (name) {
    g_autofree char *name_lower = g_utf8_strdown (name, -1);
    if (strstr (name_lower, search_lower))
      return TRUE;
  }

  /* Match against channel number */
  g_autofree char *num_str = g_strdup_printf ("%d", satwave_channel_get_number (channel));
  if (strstr (num_str, self->search_text))
    return TRUE;

  /* Match against description */
  const char *desc = satwave_channel_get_description (channel);
  if (desc) {
    g_autofree char *desc_lower = g_utf8_strdown (desc, -1);
    if (strstr (desc_lower, search_lower))
      return TRUE;
  }

  /* Match against category */
  const char *cat = satwave_channel_get_category (channel);
  if (cat) {
    g_autofree char *cat_lower = g_utf8_strdown (cat, -1);
    if (strstr (cat_lower, search_lower))
      return TRUE;
  }

  return FALSE;
}

static gboolean
favorites_filter_func (gpointer item,
                       gpointer user_data)
{
  SatwaveChannelStore *self = SATWAVE_CHANNEL_STORE (user_data);
  SatwaveChannel *channel = SATWAVE_CHANNEL (item);

  if (!self->favorites_only)
    return TRUE;

  return satwave_channel_get_is_favorite (channel);
}

static void
satwave_channel_store_dispose (GObject *object)
{
  SatwaveChannelStore *self = SATWAVE_CHANNEL_STORE (object);

  g_clear_object (&self->channels);
  g_clear_object (&self->filtered);
  g_clear_object (&self->filter);
  g_clear_object (&self->search_filter);
  g_clear_object (&self->category_filter);
  g_clear_object (&self->favorites_filter);
  g_clear_object (&self->categories);
  g_clear_pointer (&self->category, g_free);
  g_clear_pointer (&self->search_text, g_free);
  g_clear_pointer (&self->search_folded, g_free);

  G_OBJECT_CLASS (satwave_channel_store_parent_class)->dispose (object);
}

static void
satwave_channel_store_class_init (SatwaveChannelStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = satwave_channel_store_dispose;
}

static void
satwave_channel_store_init (SatwaveChannelStore *self)
{
  self->channels = g_list_store_new (SATWAVE_TYPE_CHANNEL);
  self->categories = gtk_string_list_new (NULL);

  /* Set up combined filter */
  self->filter = GTK_EVERY_FILTER (gtk_every_filter_new ());

  /* Category filter */
  self->category_filter = gtk_custom_filter_new (category_filter_func, self, NULL);
  gtk_multi_filter_append (GTK_MULTI_FILTER (self->filter),
                           GTK_FILTER (g_object_ref (self->category_filter)));

  /* Search filter */
  self->search_filter = gtk_custom_filter_new (search_filter_func, self, NULL);
  gtk_multi_filter_append (GTK_MULTI_FILTER (self->filter),
                           GTK_FILTER (g_object_ref (self->search_filter)));

  /* Favorites filter */
  self->favorites_filter = gtk_custom_filter_new (favorites_filter_func, self, NULL);
  gtk_multi_filter_append (GTK_MULTI_FILTER (self->filter),
                           GTK_FILTER (g_object_ref (self->favorites_filter)));

  /* Filtered model */
  self->filtered = gtk_filter_list_model_new (
    G_LIST_MODEL (g_object_ref (self->channels)),
    GTK_FILTER (g_object_ref (self->filter)));
}

SatwaveChannelStore *
satwave_channel_store_new (void)
{
  return g_object_new (SATWAVE_TYPE_CHANNEL_STORE, NULL);
}

void
satwave_channel_store_set_channels (SatwaveChannelStore *self,
                                    GListStore          *channels)
{
  gtk_string_list_splice (self->categories, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (self->categories)),
                          NULL);

  /* Track unique categories */
  GHashTable *seen_cats = g_hash_table_new (g_str_hash, g_str_equal);

  guint n = g_list_model_get_n_items (G_LIST_MODEL (channels));
  g_autoptr (GPtrArray) items = g_ptr_array_new_full (n, g_object_unref);

  for (guint i = 0; i < n; i++) {
    SatwaveChannel *ch = g_list_model_get_item (G_LIST_MODEL (channels), i);
    g_ptr_array_add (items, ch);

    const char *cat = satwave_channel_get_category (ch);
    if (cat && !g_hash_table_contains (seen_cats, cat)) {
      g_hash_table_add (seen_cats, (gpointer) cat);
      gtk_string_list_append (self->categories, cat);
    }
  }

  /* Single splice = one items-changed emission through the filter model
   * and grid view, instead of one per channel */
  g_list_store_splice (self->channels, 0,
                       g_list_model_get_n_items (G_LIST_MODEL (self->channels)),
                       items->pdata, items->len);

  g_hash_table_unref (seen_cats);
  g_debug ("Channel store: %u channels, %u categories",
           g_list_model_get_n_items (G_LIST_MODEL (self->channels)),
           g_list_model_get_n_items (G_LIST_MODEL (self->categories)));
}

GListModel *
satwave_channel_store_get_model (SatwaveChannelStore *self)
{
  return G_LIST_MODEL (self->channels);
}

GListModel *
satwave_channel_store_get_filtered (SatwaveChannelStore *self)
{
  return G_LIST_MODEL (self->filtered);
}

void
satwave_channel_store_set_category_filter (SatwaveChannelStore *self,
                                           const char          *category)
{
  g_free (self->category);
  self->category = g_strdup (category);
  gtk_filter_changed (GTK_FILTER (self->category_filter),
                      GTK_FILTER_CHANGE_DIFFERENT);
}

void
satwave_channel_store_set_search_filter (SatwaveChannelStore *self,
                                         const char          *search)
{
  g_free (self->search_text);
  self->search_text = g_strdup (search);
  g_free (self->search_folded);
  self->search_folded = search ? g_utf8_strdown (search, -1) : NULL;

  gtk_filter_changed (GTK_FILTER (self->search_filter),
                      GTK_FILTER_CHANGE_DIFFERENT);
}

void
satwave_channel_store_set_favorites_only (SatwaveChannelStore *self,
                                          gboolean             favorites_only)
{
  self->favorites_only = favorites_only;
  gtk_filter_changed (GTK_FILTER (self->favorites_filter),
                      GTK_FILTER_CHANGE_DIFFERENT);
}

GListModel *
satwave_channel_store_get_categories (SatwaveChannelStore *self)
{
  return G_LIST_MODEL (self->categories);
}
