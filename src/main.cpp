// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include <sonossystem.h>

#include "gnomos-application.h"

int main(int argc, char* argv[])
{
  // GNOMOS_DEBUG=<0-6> enables libnoson's own request/response logging to
  // stderr (see noson/src/private/debug.h: 0=error .. 4=proto .. 6=all).
  // Off by default since level 4+ logs full SOAP bodies, which can be noisy
  // and includes the household id.
  if (const char* level = std::getenv("GNOMOS_DEBUG"))
    NSROOT::System::Debug(std::atoi(level));

  auto app = gnomos::GnomosApplication::create();
  return app->run(argc, argv);
}
