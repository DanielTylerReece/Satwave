#include <adwaita.h>
#include <gst/gst.h>
#include "satwave-application.h"

int
main (int argc, char *argv[])
{
  g_autoptr (SatwaveApplication) app = NULL;

  gst_init (&argc, &argv);

  app = satwave_application_new ();

  return g_application_run (G_APPLICATION (app), argc, argv);
}
