// SPDX-License-Identifier: GPL-3.0-or-later

#include "i18n.h"

#include <cstdarg>

namespace gnomos
{

std::string Format(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  char* text = g_strdup_vprintf(format, args);
  va_end(args);
  std::string result = text ? text : "";
  g_free(text);
  return result;
}

}  // namespace gnomos
