#include "log.h"

#include <cstdarg>
#include <cstdio>

namespace dpfr {
namespace {
ncclDebugLogger_t gLogger = nullptr;
}

void setLogger(ncclDebugLogger_t logger) {
  gLogger = logger;
}

void logMsg(ncclDebugLogLevel level, unsigned long flags, const char* file, int line, const char* fmt, ...) {
  if (gLogger == nullptr) return;
  char buffer[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  gLogger(level, flags, file, line, "%s", buffer);
}

}  // namespace dpfr
