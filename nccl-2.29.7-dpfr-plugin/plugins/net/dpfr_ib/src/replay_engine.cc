#include "replay_engine.h"

#include "comm.h"
#include "log.h"
#include "request.h"

namespace dpfr {

ncclResult_t ReplayEngine::progress(DpfrComm* comm, DpfrRequest* request) {
  if (request == nullptr || request->state == RequestState::Free ||
      request->state == RequestState::Done || request->state == RequestState::Failed) {
    return ncclSuccess;
  }

  if (request->kind == RequestKind::Send && request->state == RequestState::NeedsQuery) {
    bool remoteDone = false;
    bool retry = false;
    ncclResult_t ret = comm->queryRemote(request->seq, &remoteDone, &retry);
    if (ret != ncclSuccess) return ret;
    if (remoteDone) {
      request->state = RequestState::Done;
      request->completedSize = static_cast<int>(request->size);
      return ncclSuccess;
    }
    if (!retry) request->state = RequestState::NeedsReplay;
  }

  if (request->kind == RequestKind::Send && request->state == RequestState::NeedsReplay) {
    ncclResult_t ret = comm->postRequest(request);
    if (ret == ncclInvalidUsage) return ncclSuccess;
    if (ret != ncclSuccess) return ret;
    request->replayed = true;
    DPFR_LOG_INFO("DPFR_IB replayed send seq=%llu plane=%d gen=%u",
                  static_cast<unsigned long long>(request->seq), request->planeId, request->generation);
  }

  return ncclSuccess;
}

}  // namespace dpfr
