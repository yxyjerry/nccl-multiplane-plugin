#ifndef DPFR_IB_LOG_H_
#define DPFR_IB_LOG_H_

#include "net.h"

namespace dpfr {

void setLogger(ncclDebugLogger_t logger);
void logMsg(ncclDebugLogLevel level, unsigned long flags, const char* file, int line, const char* fmt, ...);

#define DPFR_LOG_WARN(...) dpfr::logMsg(NCCL_LOG_WARN, NCCL_NET, __FILE__, __LINE__, __VA_ARGS__)
#define DPFR_LOG_INFO(...) dpfr::logMsg(NCCL_LOG_INFO, NCCL_NET, __FILE__, __LINE__, __VA_ARGS__)
#define DPFR_LOG_TRACE(...) dpfr::logMsg(NCCL_LOG_TRACE, NCCL_NET, __FILE__, __LINE__, __VA_ARGS__)

}  // namespace dpfr

#endif  // DPFR_IB_LOG_H_
