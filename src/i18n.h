// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// gettext for Gnomos: _() / N_() / C_() / ngettext() from GLib, plus a
// printf-style Format() for sentences with values in them — translators
// get the whole sentence ("%s added") instead of fragments glued together
// in code, so they can move the value where their language wants it.

#include <string>

#include <glib/gi18n.h>

namespace gnomos
{

std::string Format(const char* format, ...) G_GNUC_PRINTF(1, 2);

}  // namespace gnomos
