#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef void (*SatwaveCallStateChangedCb) (gboolean call_active,
                                           gpointer user_data);

typedef struct _SatwaveCallMonitor SatwaveCallMonitor;

SatwaveCallMonitor *satwave_call_monitor_new  (SatwaveCallStateChangedCb cb,
                                               gpointer                  user_data);
void                satwave_call_monitor_free (SatwaveCallMonitor *monitor);

G_END_DECLS
