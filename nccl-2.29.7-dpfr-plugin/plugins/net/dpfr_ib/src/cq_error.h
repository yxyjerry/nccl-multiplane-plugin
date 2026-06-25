#ifndef DPFR_IB_CQ_ERROR_H_
#define DPFR_IB_CQ_ERROR_H_

#include <infiniband/verbs.h>

namespace dpfr {

enum class CqErrorAction {
  Success,
  StaleNonFatal,
  TransportRecoverable,
  RequestFatal,
};

CqErrorAction classifyCqStatus(ibv_wc_status status, bool currentActiveQp);
const char* cqErrorActionName(CqErrorAction action);

}  // namespace dpfr

#endif  // DPFR_IB_CQ_ERROR_H_
