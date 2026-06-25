#include "cq_error.h"

namespace dpfr {

CqErrorAction classifyCqStatus(ibv_wc_status status, bool currentActiveQp) {
  if (status == IBV_WC_SUCCESS) return CqErrorAction::Success;
  if (!currentActiveQp) return CqErrorAction::StaleNonFatal;

  switch (status) {
    case IBV_WC_RETRY_EXC_ERR:
    case IBV_WC_RNR_RETRY_EXC_ERR:
    case IBV_WC_WR_FLUSH_ERR:
      return CqErrorAction::TransportRecoverable;
    default:
      return CqErrorAction::RequestFatal;
  }
}

const char* cqErrorActionName(CqErrorAction action) {
  switch (action) {
    case CqErrorAction::Success: return "SUCCESS";
    case CqErrorAction::StaleNonFatal: return "STALE_NON_FATAL";
    case CqErrorAction::TransportRecoverable: return "TRANSPORT_RECOVERABLE";
    case CqErrorAction::RequestFatal: return "REQUEST_FATAL";
  }
  return "UNKNOWN";
}

}  // namespace dpfr
