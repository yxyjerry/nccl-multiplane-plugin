#include "verbs_qp.h"

#include "log.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace dpfr {
namespace {

uint32_t nextPsn() {
  static uint32_t seed = static_cast<uint32_t>(time(nullptr) ^ getpid());
  seed = seed * 1103515245u + 12345u;
  return seed & 0x00ffffffu;
}

ibv_mtu minMtu(const QpEndpoint& endpoint, const WireQpInfo& remote) {
  uint32_t local = static_cast<uint32_t>(endpoint.device->portAttr.active_mtu);
  uint32_t remoteMtu = remote.mtu == 0 ? local : remote.mtu;
  return static_cast<ibv_mtu>(std::min(local, remoteMtu));
}

}  // namespace

ncclResult_t createEndpoint(PlaneDevice* device, uint32_t generation, QpEndpoint* endpoint) {
  *endpoint = QpEndpoint();
  endpoint->device = device;
  endpoint->generation = generation;
  endpoint->psn = nextPsn();

  endpoint->cq = ibv_create_cq(device->context, NCCL_NET_MAX_REQUESTS * 4 + 16, nullptr, nullptr, 0);
  if (endpoint->cq == nullptr) {
    DPFR_LOG_WARN("DPFR_IB failed to create CQ for plane %d", device->planeId);
    return ncclSystemError;
  }

  ibv_qp_init_attr init = {};
  init.send_cq = endpoint->cq;
  init.recv_cq = endpoint->cq;
  init.qp_type = IBV_QPT_RC;
  init.cap.max_send_wr = NCCL_NET_MAX_REQUESTS * 4 + 16;
  init.cap.max_recv_wr = NCCL_NET_MAX_REQUESTS * 4 + 16;
  init.cap.max_send_sge = 1;
  init.cap.max_recv_sge = 1;
  endpoint->qp = ibv_create_qp(device->pd, &init);
  if (endpoint->qp == nullptr) {
    DPFR_LOG_WARN("DPFR_IB failed to create QP for plane %d", device->planeId);
    destroyEndpoint(endpoint);
    return ncclSystemError;
  }

  ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = device->port;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  if (ibv_modify_qp(endpoint->qp, &attr, flags) != 0) {
    DPFR_LOG_WARN("DPFR_IB failed to move QP to INIT for plane %d", device->planeId);
    destroyEndpoint(endpoint);
    return ncclSystemError;
  }

  return ncclSuccess;
}

void destroyEndpoint(QpEndpoint* endpoint) {
  if (endpoint == nullptr) return;
  if (endpoint->qp) ibv_destroy_qp(endpoint->qp);
  if (endpoint->cq) ibv_destroy_cq(endpoint->cq);
  endpoint->qp = nullptr;
  endpoint->cq = nullptr;
  endpoint->ready = false;
}

WireQpInfo endpointInfo(const QpEndpoint& endpoint) {
  WireQpInfo info;
  info.planeId = static_cast<uint32_t>(endpoint.device->planeId);
  info.generation = endpoint.generation;
  info.qpn = endpoint.qp ? endpoint.qp->qp_num : 0;
  info.lid = endpoint.device->portAttr.lid;
  info.psn = endpoint.psn;
  info.mtu = static_cast<uint32_t>(endpoint.device->portAttr.active_mtu);
  info.gidIndex = static_cast<uint32_t>(endpoint.device->gidIndex);
  info.linkLayer = endpoint.device->linkLayer;
  memcpy(info.gid, &endpoint.device->gid, sizeof(info.gid));
  return info;
}

ncclResult_t endpointToRtrRts(QpEndpoint* endpoint, const WireQpInfo& remote) {
  ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = minMtu(*endpoint, remote);
  attr.dest_qp_num = remote.qpn;
  attr.rq_psn = remote.psn;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;
  attr.ah_attr.port_num = endpoint->device->port;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  if (endpoint->device->linkLayer == IBV_LINK_LAYER_ETHERNET || remote.linkLayer == IBV_LINK_LAYER_ETHERNET) {
    attr.ah_attr.is_global = 1;
    memcpy(&attr.ah_attr.grh.dgid, remote.gid, sizeof(remote.gid));
    attr.ah_attr.grh.sgid_index = endpoint->device->gidIndex;
    attr.ah_attr.grh.hop_limit = 255;
  } else {
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = static_cast<uint16_t>(remote.lid);
  }

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  if (ibv_modify_qp(endpoint->qp, &attr, flags) != 0) {
    DPFR_LOG_WARN("DPFR_IB failed to move QP to RTR plane %d gen %u", endpoint->device->planeId, endpoint->generation);
    return ncclSystemError;
  }

  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = endpoint->psn;
  attr.max_rd_atomic = 1;
  flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  if (ibv_modify_qp(endpoint->qp, &attr, flags) != 0) {
    DPFR_LOG_WARN("DPFR_IB failed to move QP to RTS plane %d gen %u", endpoint->device->planeId, endpoint->generation);
    return ncclSystemError;
  }

  endpoint->ready = true;
  return ncclSuccess;
}

ncclResult_t postSend(QpEndpoint* endpoint, void* data, size_t size, uint32_t lkey, uint64_t wrId) {
  if (size > UINT32_MAX) return ncclInvalidArgument;
  ibv_sge sge = {};
  sge.addr = reinterpret_cast<uintptr_t>(data);
  sge.length = static_cast<uint32_t>(size);
  sge.lkey = lkey;

  ibv_send_wr wr = {};
  wr.wr_id = wrId;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;

  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(endpoint->qp, &wr, &bad) != 0) return ncclSystemError;
  return ncclSuccess;
}

ncclResult_t postRecv(QpEndpoint* endpoint, void* data, size_t size, uint32_t lkey, uint64_t wrId) {
  if (size > UINT32_MAX) return ncclInvalidArgument;
  ibv_sge sge = {};
  sge.addr = reinterpret_cast<uintptr_t>(data);
  sge.length = static_cast<uint32_t>(size);
  sge.lkey = lkey;

  ibv_recv_wr wr = {};
  wr.wr_id = wrId;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  ibv_recv_wr* bad = nullptr;
  if (ibv_post_recv(endpoint->qp, &wr, &bad) != 0) return ncclSystemError;
  return ncclSuccess;
}

ncclResult_t postEmptySend(QpEndpoint* endpoint, uint64_t wrId) {
  ibv_send_wr wr = {};
  wr.wr_id = wrId;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  ibv_send_wr* bad = nullptr;
  if (ibv_post_send(endpoint->qp, &wr, &bad) != 0) return ncclSystemError;
  return ncclSuccess;
}

ncclResult_t postEmptyRecv(QpEndpoint* endpoint, uint64_t wrId) {
  ibv_recv_wr wr = {};
  wr.wr_id = wrId;
  ibv_recv_wr* bad = nullptr;
  if (ibv_post_recv(endpoint->qp, &wr, &bad) != 0) return ncclSystemError;
  return ncclSuccess;
}

int pollEndpoint(QpEndpoint* endpoint, ibv_wc* wc, int maxWc) {
  if (endpoint == nullptr || endpoint->cq == nullptr) return 0;
  return ibv_poll_cq(endpoint->cq, maxWc, wc);
}

}  // namespace dpfr
