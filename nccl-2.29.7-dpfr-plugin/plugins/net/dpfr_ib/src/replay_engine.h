#ifndef DPFR_IB_REPLAY_ENGINE_H_
#define DPFR_IB_REPLAY_ENGINE_H_

#include "net.h"

namespace dpfr {

class DpfrComm;
struct DpfrRequest;

class ReplayEngine {
 public:
  ncclResult_t progress(DpfrComm* comm, DpfrRequest* request);
};

}  // namespace dpfr

#endif  // DPFR_IB_REPLAY_ENGINE_H_
