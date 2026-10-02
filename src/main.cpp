// SPDX-License-Identifier: GPL-3.0-or-later

#include <clocale>
#include <cstdlib>

#include <glib/gi18n.h>

#include <sonossystem.h>

#include "config.h"
#include "gnomos-application.h"

int main(int argc, char* argv[])
{
  // GNOMOS_DEBUG=<0-6> enables libnoson's own request/response logging to
  // stderr (see noson/src/private/debug.h: 0=error .. 4=proto .. 6=all).
  // Off by default since level 4+ logs full SOAP bodies, which can be noisy
  // and includes the household id.
  if (const char* level = std::getenv("GNOMOS_DEBUG"))
    NSROOT::System::Debug(std::atoi(level));

  // UI strings are English in the source and translated via gettext
  // (po/). A build run straight from the source tree finds no installed
  // catalogs and stays English, unless GNOMOS_LOCALEDIR points at a
  // directory with compiled .mo files (e.g. _build/po).
  std::setlocale(LC_ALL, "");
  const char* localedir = std::getenv("GNOMOS_LOCALEDIR");
  bindtextdomain(GETTEXT_PACKAGE, localedir ? localedir : LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  auto app = gnomos::GnomosApplication::create();
  return app->run(argc, argv);
}
