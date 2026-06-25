#ifndef DPFR_IB_VERBS_QP_H_
#define DPFR_IB_VERBS_QP_H_

#include "control_wire.h"
#include "verbs_device.h"

#include <infiniband/verbs.h>

#include <cstdint>

namespace dpfr {

struct QpEndpoint {
  PlaneDevice* device = nullptr;
  ibv_cq* cq = nullptr;
  ibv_qp* qp = nullptr;
  uint32_t generation = 0;
  uint32_t psn = 0;
  bool ready = false;
};

ncclResult_t createEndpoint(PlaneDevice* device, uint32_t generation, QpEndpoint* endpoint);
void destroyEndpoint(QpEndpoint* endpoint);
WireQpInfo endpointInfo(const QpEndpoint& endpoint);
ncclResult_t endpointToRtrRts(QpEndpoint* endpoint, const WireQpInfo& remote);
ncclResult_t postSend(QpEndpoint* endpoint, void* data, size_t size, uint32_t lkey, uint64_t wrId);
ncclResult_t postRecv(QpEndpoint* endpoint, void* data, size_t size, uint32_t lkey, uint64_t wrId);
ncclResult_t postEmptySend(QpEndpoint* endpoint, uint64_t wrId);
ncclResult_t postEmptyRecv(QpEndpoint* endpoint, uint64_t wrId);
int pollEndpoint(QpEndpoint* endpoint, ibv_wc* wc, int maxWc);

}  // namespace dpfr

#endif  // DPFR_IB_VERBS_QP_H_
