#include "comm.h"

#include "cq_error.h"
#include "log.h"
#include "mr.h"
#include "verbs_qp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <thread>

namespace dpfr {
namespace {

constexpr int kHandshakeTimeoutMs = 10000;
constexpr int kControlProgressBudget = 8;
constexpr size_t kMaxPluginBytes = 1024ULL * 1024ULL * 1024ULL;

enum class PolledQpRole {
  Active,
  Shadow,
  Retired,
};

bool planeAllowsQpRecovery(const PlaneSnapshot& plane) {
  return plane.health == PlaneHealth::Recovering || plane.health == PlaneHealth::Healthy;
}

bool slotAtGeneration(const QpSlot& slot, uint32_t generation) {
  return slot.state == QpSlotState::Active && slot.active.qp != nullptr &&
         slot.active.ready && slot.active.generation == generation;
}

int tcpConnect(const DpfrNetHandle& handle) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(handle.port);
  addr.sin_addr.s_addr = handle.ipv4;
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int findSlotByPlane(std::vector<QpSlot>& slots, int planeId) {
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i].planeId == planeId) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

DpfrComm::DpfrComm(DpfrContext* context, bool sendSide) : context_(context), sendSide_(sendSide) {
  for (int i = 0; i < NCCL_NET_MAX_REQUESTS; ++i) {
    requests_[i].index = i;
    requests_[i].owner = this;
  }
}

DpfrComm::~DpfrComm() {
  close();
}

ncclResult_t DpfrComm::createInitialSlots() {
  slots_.clear();
  int index = 0;
  for (PlaneDevice& device : context_->devices()) {
    QpSlot slot;
    slot.planeId = device.planeId;
    slot.planeIndex = index++;
    slot.state = QpSlotState::Rebuilding;
    uint32_t generation = context_->planeTable().generation(device.planeId);
    ncclResult_t ret = createEndpoint(&device, generation, &slot.active);
    if (ret != ncclSuccess) return ret;
    slots_.push_back(slot);
  }
  return slots_.empty() ? ncclSystemError : ncclSuccess;
}

ncclResult_t DpfrComm::exportQps(WireMsgType type, WireMsg* msg) const {
  *msg = makeMessage(type);
  msg->nQps = static_cast<uint32_t>(slots_.size());
  if (msg->nQps > kMaxPlanes) return ncclInvalidUsage;
  for (size_t i = 0; i < slots_.size(); ++i) msg->qps[i] = endpointInfo(slots_[i].active);
  return ncclSuccess;
}

ncclResult_t DpfrComm::importRemoteQps(const WireMsg& msg) {
  if (!validMessage(msg)) return ncclRemoteError;
  for (uint32_t i = 0; i < msg.nQps; ++i) {
    int slotIndex = findSlotByPlane(slots_, static_cast<int>(msg.qps[i].planeId));
    if (slotIndex < 0) return ncclRemoteError;
    QpSlot& slot = slots_[slotIndex];
    if (slot.active.generation != msg.qps[i].generation) {
      DPFR_LOG_WARN("DPFR_IB initial generation mismatch plane=%d local=%u remote=%u",
                    slot.planeId, slot.active.generation, msg.qps[i].generation);
      return ncclRemoteError;
    }
    ncclResult_t ret = endpointToRtrRts(&slot.active, msg.qps[i]);
    if (ret != ncclSuccess) return ret;
    slot.state = QpSlotState::Active;
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::initAsConnector(const DpfrNetHandle& handle) {
  int fd = tcpConnect(handle);
  if (fd < 0) return ncclSystemError;
  peer_.reset(fd);
  ncclResult_t ret = createInitialSlots();
  if (ret != ncclSuccess) return ret;

  WireMsg initReq;
  ret = exportQps(WireMsgType::InitReq, &initReq);
  if (ret == ncclSuccess) ret = peer_.sendMessage(initReq);
  WireMsg initResp;
  if (ret == ncclSuccess) ret = peer_.recvMessage(&initResp, kHandshakeTimeoutMs);
  if (ret == ncclSuccess && initResp.type != WireMsgType::InitResp) ret = ncclRemoteError;
  if (ret == ncclSuccess) ret = importRemoteQps(initResp);
  if (ret != ncclSuccess) return ret;

  context_->registerComm(this);
  return ncclSuccess;
}

ncclResult_t DpfrComm::initFromAcceptedFd(int fd, WireMsg* initReq) {
  peer_.reset(fd);
  ncclResult_t ret = createInitialSlots();
  if (ret == ncclSuccess) ret = importRemoteQps(*initReq);
  WireMsg initResp;
  if (ret == ncclSuccess) ret = exportQps(WireMsgType::InitResp, &initResp);
  if (ret == ncclSuccess) ret = peer_.sendMessage(initResp);
  if (ret != ncclSuccess) return ret;

  context_->registerComm(this);
  return ncclSuccess;
}

DpfrRequest* DpfrComm::allocRequest() {
  for (DpfrRequest& req : requests_) {
    if (req.state == RequestState::Free || req.state == RequestState::Done || req.state == RequestState::Failed) {
      req = DpfrRequest();
      req.index = static_cast<int>(&req - requests_.data());
      req.owner = this;
      return &req;
    }
  }
  return nullptr;
}

bool DpfrComm::hasOutstandingLocked() const {
  for (const DpfrRequest& req : requests_) {
    if (req.state == RequestState::Posted || req.state == RequestState::NeedsQuery || req.state == RequestState::NeedsReplay) return true;
  }
  return false;
}

void DpfrComm::releaseDoneRequestsLocked() {}

ncclResult_t DpfrComm::postRequest(DpfrRequest* req) {
  int slotIndex = selector_.select(&slots_, context_->planeTable(), req->seq);
  if (slotIndex < 0) return ncclInvalidUsage;
  QpSlot& slot = slots_[slotIndex];
  ibv_mr* mr = mrForPlane(req->mhandle, context_->devices(), slot.planeId);
  if (mr == nullptr) return ncclInvalidArgument;

  uint64_t wrId = makeWrId(req->index, slot.active.generation);
  ncclResult_t ret = req->kind == RequestKind::Send
                         ? postSend(&slot.active, req->data, req->size, mr->lkey, wrId)
                         : postRecv(&slot.active, req->data, req->size, mr->lkey, wrId);
  if (ret != ncclSuccess) return ret;

  req->slotIndex = slotIndex;
  req->planeId = slot.planeId;
  req->generation = slot.active.generation;
  req->state = RequestState::Posted;
  return ncclSuccess;
}

ncclResult_t DpfrComm::isend(void* data, size_t size, int tag, void* mhandle, void** request) {
  *request = nullptr;
  if (size > kMaxPluginBytes) return ncclInvalidArgument;
  ncclResult_t progressRet = progress();
  if (progressRet != ncclSuccess) return progressRet;
  std::lock_guard<std::mutex> lock(mutex_);
  if (failed_) return ncclSystemError;
  if (hasOutstandingLocked()) return ncclSuccess;
  DpfrRequest* req = allocRequest();
  if (req == nullptr) return ncclSuccess;
  req->kind = RequestKind::Send;
  req->seq = nextSeq_++;
  req->data = data;
  req->size = size;
  req->tag = tag;
  req->mhandle = mhandle;
  ncclResult_t ret = postRequest(req);
  if (ret == ncclInvalidUsage) {
    req->state = RequestState::Free;
    return ncclSuccess;
  }
  if (ret != ncclSuccess) {
    req->state = RequestState::Failed;
    return ret;
  }
  *request = req;
  return ncclSuccess;
}

ncclResult_t DpfrComm::irecv(int n, void** data, size_t* sizes, int* tags, void** mhandles, void** request) {
  *request = nullptr;
  if (n != 1 || sizes[0] > kMaxPluginBytes) return ncclInvalidUsage;
  ncclResult_t progressRet = progress();
  if (progressRet != ncclSuccess) return progressRet;
  std::lock_guard<std::mutex> lock(mutex_);
  if (failed_) return ncclSystemError;
  if (hasOutstandingLocked()) return ncclSuccess;
  DpfrRequest* req = allocRequest();
  if (req == nullptr) return ncclSuccess;
  req->kind = RequestKind::Recv;
  req->seq = nextSeq_++;
  req->data = data[0];
  req->size = sizes[0];
  req->tag = tags ? tags[0] : 0;
  req->mhandle = mhandles[0];
  ncclResult_t ret = postRequest(req);
  if (ret == ncclInvalidUsage) {
    req->state = RequestState::Free;
    return ncclSuccess;
  }
  if (ret != ncclSuccess) {
    req->state = RequestState::Failed;
    return ret;
  }
  *request = req;
  return ncclSuccess;
}

ncclResult_t DpfrComm::iflush(int n, void** data, int* sizes, void** mhandles, void** request) {
  *request = nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  DpfrRequest* req = allocRequest();
  if (req == nullptr) return ncclSuccess;
  req->kind = RequestKind::Flush;
  req->state = RequestState::Done;
  req->completedSize = n > 0 && sizes ? sizes[0] : 0;
  *request = req;
  return ncclSuccess;
}

ncclResult_t DpfrComm::progressCqs() {
  ibv_wc wc[8];
  auto pollOne = [&](QpEndpoint* endpoint, QpSlot* slot, PolledQpRole role) -> ncclResult_t {
    int n = 0;
    while ((n = pollEndpoint(endpoint, wc, 8)) > 0) {
      for (int i = 0; i < n; ++i) {
        if (isInternalWrId(wc[i].wr_id)) {
          recordInternalCqe(slot, wc[i].wr_id, wc[i].status);
          continue;
        }
        int idx = wrRequestIndex(wc[i].wr_id);
        uint32_t gen = wrGeneration(wc[i].wr_id);
        if (idx < 0 || idx >= NCCL_NET_MAX_REQUESTS) continue;
        DpfrRequest& req = requests_[idx];
        if (req.generation != gen) {
          DPFR_LOG_TRACE("DPFR_IB ignored stale CQE seq=%llu cqeGen=%u reqGen=%u",
                         static_cast<unsigned long long>(req.seq), gen, req.generation);
          continue;
        }
        bool currentActiveQp = role == PolledQpRole::Active && slot->state == QpSlotState::Active &&
                               endpoint == &slot->active && endpoint->generation == slot->active.generation &&
                               req.planeId == slot->planeId && req.generation == slot->active.generation;
        CqErrorAction action = classifyCqStatus(wc[i].status, currentActiveQp);
        switch (action) {
          case CqErrorAction::Success:
            req.completedSize = static_cast<int>(wc[i].byte_len);
            req.state = RequestState::Done;
            if (req.kind == RequestKind::Recv) markRecvDone(req.seq);
            destroyRetiredIfUnused(slot);
            break;
          case CqErrorAction::StaleNonFatal:
            DPFR_LOG_TRACE("DPFR_IB nonfatal stale CQE plane=%d gen=%u status=%d state=%s",
                           slot->planeId, gen, wc[i].status, qpSlotStateName(slot->state));
            if (req.state == RequestState::Posted) {
              req.state = req.kind == RequestKind::Send ? RequestState::NeedsQuery : RequestState::NeedsReplay;
            }
            destroyRetiredIfUnused(slot);
            break;
          case CqErrorAction::TransportRecoverable:
            DPFR_LOG_WARN("DPFR_IB recoverable CQ error plane=%d gen=%u status=%d",
                          slot->planeId, gen, wc[i].status);
            {
              ncclResult_t markRet = markLocalPlaneBrokenFromCq(slot, gen, wc[i].status);
              if (markRet != ncclSuccess) return markRet;
            }
            if (req.state == RequestState::Posted) {
              req.state = req.kind == RequestKind::Send ? RequestState::NeedsQuery : RequestState::NeedsReplay;
            }
            break;
          case CqErrorAction::RequestFatal:
            DPFR_LOG_WARN("DPFR_IB fatal CQ error plane=%d gen=%u status=%d",
                          req.planeId, gen, wc[i].status);
            req.state = RequestState::Failed;
            failed_ = true;
            return ncclSystemError;
        }
      }
    }
    if (n < 0) {
      failed_ = true;
      return ncclSystemError;
    }
    return ncclSuccess;
  };

  for (QpSlot& slot : slots_) {
    ncclResult_t ret = pollOne(&slot.active, &slot, PolledQpRole::Active);
    if (ret != ncclSuccess) return ret;
    ret = pollOne(&slot.shadow, &slot, PolledQpRole::Shadow);
    if (ret != ncclSuccess) return ret;
    for (QpEndpoint& retired : slot.retired) {
      ret = pollOne(&retired, &slot, PolledQpRole::Retired);
      if (ret != ncclSuccess) return ret;
    }
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::progressControl() {
  if (!peer_.valid()) return ncclRemoteError;
  for (int i = 0; i < kControlProgressBudget; ++i) {
    WireMsg msg;
    bool got = false;
    ncclResult_t ret = peer_.progressRecv(&msg, &got);
    if (ret != ncclSuccess) return ret;
    if (!got) return ncclSuccess;
    ret = handleControl(msg);
    if (ret != ncclSuccess) return ret;
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::markLocalPlaneBrokenFromCq(QpSlot* slot, uint32_t generation, ibv_wc_status status) {
  if (slot == nullptr) return ncclInternalError;
  PlaneSnapshot snapshot;
  if (!context_->planeTable().markBroken(slot->planeId, "local-cq-error", &snapshot)) {
    failed_ = true;
    return ncclInternalError;
  }

  for (QpSlot& candidate : slots_) {
    if (candidate.planeId != slot->planeId) continue;
    candidate.abortRecovery();
    candidate.quarantine();
    for (DpfrRequest& req : requests_) {
      if (req.planeId == candidate.planeId && req.state == RequestState::Posted) {
        req.state = req.kind == RequestKind::Send ? RequestState::NeedsQuery : RequestState::NeedsReplay;
      }
    }
  }

  DPFR_LOG_INFO("DPFR_IB local CQ error marked plane broken plane=%d oldGen=%u newGen=%u status=%d",
                slot->planeId, generation, snapshot.generation, status);
  ncclResult_t ret = sendPlaneEvent(slot->planeId, snapshot.generation, false);
  if (ret != ncclSuccess) {
    failed_ = true;
    return ret;
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::ensureRecoveryShadow(QpSlot* slot, uint32_t generation) {
  if (slot == nullptr) return ncclInternalError;
  if (slot->shadow.qp != nullptr && slot->shadow.generation != generation) {
    destroyEndpoint(&slot->shadow);
  }
  if (slot->shadow.qp != nullptr) return ncclSuccess;

  PlaneDevice* device = findPlaneDevice(&context_->devices(), slot->planeId);
  if (device == nullptr) return ncclInternalError;
  ncclResult_t ret = refreshPlaneDevice(device);
  if (ret != ncclSuccess) return ret;
  ret = createEndpoint(device, generation, &slot->shadow);
  if (ret != ncclSuccess) return ret;
  slot->resetProbe(generation);
  slot->state = QpSlotState::Rebuilding;
  DPFR_LOG_INFO("DPFR_IB created shadow QP plane=%d gen=%u after metadata refresh", slot->planeId, generation);
  return ncclSuccess;
}

void DpfrComm::recordInternalCqe(QpSlot* slot, uint64_t wrId, ibv_wc_status status) {
  if (slot == nullptr || !isInternalWrId(wrId)) return;
  uint32_t generation = internalWrGeneration(wrId);
  if (generation != slot->probeGeneration) return;
  if (status != IBV_WC_SUCCESS) {
    slot->probeFailed = true;
    DPFR_LOG_WARN("DPFR_IB internal recovery probe CQE failed plane=%d gen=%u status=%d",
                  slot->planeId, generation, status);
    return;
  }
  switch (internalWrKind(wrId)) {
    case InternalWrKind::RecoverySend:
      slot->probeSendDone = true;
      break;
    case InternalWrKind::RecoveryRecv:
      slot->probeRecvDone = true;
      break;
  }
}

ncclResult_t DpfrComm::waitForInternalProbe(QpSlot* slot, InternalWrKind kind, uint32_t generation, int timeoutMs) {
  if (slot == nullptr || slot->shadow.qp == nullptr || slot->probeGeneration != generation) return ncclInternalError;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  ibv_wc wc[4];
  while (std::chrono::steady_clock::now() < deadline) {
    if (slot->probeFailed) return ncclInvalidUsage;
    if (kind == InternalWrKind::RecoverySend && slot->probeSendDone) return ncclSuccess;
    if (kind == InternalWrKind::RecoveryRecv && slot->probeRecvDone) return ncclSuccess;

    int n = pollEndpoint(&slot->shadow, wc, 4);
    if (n < 0) return ncclSystemError;
    for (int i = 0; i < n; ++i) {
      if (isInternalWrId(wc[i].wr_id)) {
        recordInternalCqe(slot, wc[i].wr_id, wc[i].status);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  DPFR_LOG_WARN("DPFR_IB internal recovery probe timed out plane=%d gen=%u", slot->planeId, generation);
  return ncclInvalidUsage;
}

ncclResult_t DpfrComm::progressRecovery() {
  if (!sendSide_) return ncclSuccess;
  for (QpSlot& slot : slots_) {
    PlaneSnapshot plane;
    if (!context_->planeTable().get(slot.planeId, &plane) || !planeAllowsQpRecovery(plane)) continue;
    if (slotAtGeneration(slot, plane.generation)) continue;
    ncclResult_t ret = ensureRecoveryShadow(&slot, plane.generation);
    if (ret == ncclInvalidUsage) {
      DPFR_LOG_INFO("DPFR_IB recovery not ready locally plane=%d gen=%u", slot.planeId, plane.generation);
      continue;
    }
    if (ret != ncclSuccess) return ret;

    WireMsg req = makeMessage(WireMsgType::RecoveryReq);
    req.planeId = static_cast<uint32_t>(slot.planeId);
    req.generation = plane.generation;
    req.nQps = 1;
    req.qps[0] = endpointInfo(slot.shadow);
    ret = peer_.sendMessage(req);
    if (ret != ncclSuccess) return ret;

    WireMsg resp;
    for (;;) {
      ret = peer_.recvMessage(&resp, kHandshakeTimeoutMs);
      if (ret != ncclSuccess) return ret;
      if (resp.type == WireMsgType::RecoveryResp && resp.planeId == req.planeId && resp.generation == req.generation) break;
      ret = handleControl(resp);
      if (ret != ncclSuccess) return ret;
    }
    if (resp.status == WireStatus::Retry) {
      DPFR_LOG_INFO("DPFR_IB recovery peer not ready plane=%d gen=%u", slot.planeId, plane.generation);
      slot.abortRecovery();
      continue;
    }
    if (resp.status != WireStatus::Ok || resp.nQps != 1 || !validQpInfo(resp.qps[0]) ||
        resp.qps[0].planeId != req.planeId || resp.qps[0].generation != req.generation) {
      return ncclRemoteError;
    }

    ret = endpointToRtrRts(&slot.shadow, resp.qps[0]);
    if (ret != ncclSuccess) return ret;
    slot.resetProbe(plane.generation);
    slot.state = QpSlotState::ReadyPending;
    ret = postEmptySend(&slot.shadow, makeInternalWrId(InternalWrKind::RecoverySend, plane.generation));
    if (ret != ncclSuccess) return ret;
    ret = waitForInternalProbe(&slot, InternalWrKind::RecoverySend, plane.generation, kHandshakeTimeoutMs);
    if (ret != ncclSuccess) {
      DPFR_LOG_WARN("DPFR_IB recovery probe send failed plane=%d gen=%u", slot.planeId, plane.generation);
      slot.abortRecovery();
      continue;
    }

    WireMsg ready = makeMessage(WireMsgType::RecoveryReady);
    ready.planeId = static_cast<uint32_t>(slot.planeId);
    ready.generation = plane.generation;
    ret = peer_.sendMessage(ready);
    if (ret != ncclSuccess) return ret;

    WireMsg ack;
    for (;;) {
      ret = peer_.recvMessage(&ack, kHandshakeTimeoutMs);
      if (ret != ncclSuccess) return ret;
      if (ack.type == WireMsgType::RecoveryAck && ack.planeId == ready.planeId && ack.generation == ready.generation) break;
      ret = handleControl(ack);
      if (ret != ncclSuccess) return ret;
    }
    if (ack.status != WireStatus::Ok) {
      slot.abortRecovery();
      continue;
    }

    PlaneSnapshot current;
    if (!context_->planeTable().get(slot.planeId, &current) ||
        !planeAllowsQpRecovery(current) || current.generation != plane.generation) {
      DPFR_LOG_WARN("DPFR_IB recovery commit skipped for stale plane state plane=%d reqGen=%u currentGen=%u current=%s",
                    slot.planeId, plane.generation, current.generation, planeHealthName(current.health));
      slot.abortRecovery();
      continue;
    }
    ret = commitShadow(slot.planeId, plane.generation);
    if (ret != ncclSuccess) return ret;
    if (!context_->planeTable().markHealthyIfGeneration(slot.planeId, plane.generation, "qp-recovery", &current)) {
      slot.quarantine();
      continue;
    }
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::progress() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (failed_) return ncclSystemError;
  ncclResult_t ret = progressControl();
  if (ret != ncclSuccess) return ret;
  ret = progressCqs();
  if (ret != ncclSuccess) return ret;
  for (DpfrRequest& req : requests_) {
    ret = replay_.progress(this, &req);
    if (ret != ncclSuccess) return ret;
  }
  return progressRecovery();
}

ncclResult_t DpfrComm::test(DpfrRequest* request, int* done, int* size) {
  if (done) *done = 0;
  ncclResult_t progressRet = progress();
  if (progressRet != ncclSuccess) return progressRet;
  std::lock_guard<std::mutex> lock(mutex_);
  if (request == nullptr) return ncclInvalidArgument;
  if (request->state == RequestState::Failed) return ncclSystemError;
  if (request->state == RequestState::Done) {
    if (done) *done = 1;
    if (size) *size = request->completedSize;
    request->state = RequestState::Free;
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::sendPlaneEvent(int planeId, uint32_t generation, bool recovered) {
  WireMsg msg = makeMessage(WireMsgType::PlaneEvent);
  msg.planeId = static_cast<uint32_t>(planeId);
  msg.generation = generation;
  msg.done = recovered ? 1 : 0;
  return peer_.valid() ? peer_.sendMessage(msg) : ncclSuccess;
}

ncclResult_t DpfrComm::handleControl(const WireMsg& msg) {
  switch (msg.type) {
    case WireMsgType::PlaneEvent: {
      int planeId = static_cast<int>(msg.planeId);
      bool recovered = msg.done != 0;
      PlaneSnapshot snapshot;
      bool applied = false;
      PlaneHealth health = recovered ? PlaneHealth::Recovering : PlaneHealth::Broken;
      if (!context_->planeTable().applyRemoteEvent(planeId, health, msg.generation, "peer-control", &snapshot, &applied)) {
        DPFR_LOG_WARN("DPFR_IB ignored invalid peer plane event plane=%d gen=%u", planeId, msg.generation);
        return ncclSuccess;
      }
      if (!applied) {
        DPFR_LOG_TRACE("DPFR_IB ignored stale peer plane event plane=%d eventGen=%u currentGen=%u current=%s",
                       planeId, msg.generation, snapshot.generation, planeHealthName(snapshot.health));
        return ncclSuccess;
      }
      if (recovered) {
        for (QpSlot& slot : slots_) {
          if (slot.planeId != planeId) continue;
          if (slot.shadow.qp != nullptr && slot.shadow.generation != snapshot.generation) slot.abortRecovery();
          if (slot.active.generation != snapshot.generation || slot.state != QpSlotState::Active) {
            slot.state = QpSlotState::Rebuilding;
          }
        }
      } else {
        for (QpSlot& slot : slots_) {
          if (slot.planeId != planeId) continue;
          slot.abortRecovery();
          slot.quarantine();
          for (DpfrRequest& req : requests_) {
            if (req.planeId == planeId && req.state == RequestState::Posted) {
              req.state = req.kind == RequestKind::Send ? RequestState::NeedsQuery : RequestState::NeedsReplay;
            }
          }
        }
      }
      return ncclSuccess;
    }
    case WireMsgType::Query:
      return handleQuery(msg);
    case WireMsgType::RecoveryReq:
      return handleRecoveryReq(msg);
    case WireMsgType::RecoveryReady:
      return handleRecoveryReady(msg);
    case WireMsgType::RecoveryAck:
      return ncclSuccess;
    case WireMsgType::Error:
      failed_ = true;
      return ncclRemoteError;
    default:
      return ncclSuccess;
  }
}

ncclResult_t DpfrComm::handleQuery(const WireMsg& msg) {
  WireMsg resp = makeMessage(WireMsgType::QueryResp);
  resp.seq = msg.seq;
  if (recvDone(msg.seq)) {
    resp.done = 1;
    return peer_.sendMessage(resp);
  }

  bool reposted = false;
  ncclResult_t ret = repostRecvForSeq(msg.seq, &reposted);
  if (ret != ncclSuccess) {
    resp.status = WireStatus::Failed;
  } else if (!reposted) {
    resp.status = WireStatus::Retry;
  } else {
    resp.done = 0;
  }
  return peer_.sendMessage(resp);
}

ncclResult_t DpfrComm::queryRemote(uint64_t seq, bool* done, bool* retry) {
  *done = false;
  *retry = false;
  WireMsg query = makeMessage(WireMsgType::Query);
  query.seq = seq;
  ncclResult_t ret = peer_.sendMessage(query);
  if (ret != ncclSuccess) return ret;
  for (int i = 0; i < 50; ++i) {
    WireMsg resp;
    ret = peer_.recvMessage(&resp, 100);
    if (ret == ncclInternalError) {
      *retry = true;
      return ncclSuccess;
    }
    if (ret != ncclSuccess) return ret;
    if (resp.type == WireMsgType::QueryResp && resp.seq == seq) {
      if (resp.status == WireStatus::Retry) *retry = true;
      else if (resp.status == WireStatus::Failed) return ncclRemoteError;
      *done = resp.done != 0;
      return ncclSuccess;
    }
    ret = handleControl(resp);
    if (ret != ncclSuccess) return ret;
  }
  *retry = true;
  return ncclSuccess;
}

ncclResult_t DpfrComm::repostRecvForSeq(uint64_t seq, bool* reposted) {
  *reposted = false;
  for (DpfrRequest& req : requests_) {
    if (req.kind != RequestKind::Recv || req.seq != seq) continue;
    if (req.state == RequestState::Done) return ncclSuccess;
    req.state = RequestState::NeedsReplay;
    ncclResult_t ret = postRequest(&req);
    if (ret == ncclInvalidUsage) return ncclSuccess;
    if (ret != ncclSuccess) return ret;
    *reposted = true;
    return ncclSuccess;
  }
  return ncclSuccess;
}

ncclResult_t DpfrComm::handleRecoveryReq(const WireMsg& msg) {
  int slotIndex = findSlotByPlane(slots_, static_cast<int>(msg.planeId));
  if (slotIndex < 0 || msg.nQps != 1 || !validQpInfo(msg.qps[0]) ||
      msg.qps[0].planeId != msg.planeId || msg.qps[0].generation != msg.generation) {
    return ncclRemoteError;
  }
  PlaneSnapshot plane;
  if (!context_->planeTable().applyRemoteEvent(static_cast<int>(msg.planeId), PlaneHealth::Recovering,
                                               msg.generation, "recovery-req", &plane, nullptr)) {
    return ncclRemoteError;
  }
  if (plane.generation != msg.generation || !planeAllowsQpRecovery(plane)) {
    WireMsg resp = makeMessage(WireMsgType::RecoveryResp);
    resp.planeId = msg.planeId;
    resp.generation = msg.generation;
    resp.status = WireStatus::Retry;
    peer_.sendMessage(resp);
    return ncclSuccess;
  }
  QpSlot& slot = slots_[slotIndex];
  if (slot.shadow.qp != nullptr) slot.abortRecovery();
  ncclResult_t ret = ensureRecoveryShadow(&slot, msg.generation);
  if (ret != ncclSuccess) {
    WireMsg resp = makeMessage(WireMsgType::RecoveryResp);
    resp.planeId = msg.planeId;
    resp.generation = msg.generation;
    resp.status = ret == ncclInvalidUsage ? WireStatus::Retry : WireStatus::Failed;
    peer_.sendMessage(resp);
    return ret == ncclInvalidUsage ? ncclSuccess : ret;
  }

  ret = endpointToRtrRts(&slot.shadow, msg.qps[0]);
  if (ret != ncclSuccess) {
    WireMsg resp = makeMessage(WireMsgType::RecoveryResp);
    resp.planeId = msg.planeId;
    resp.generation = msg.generation;
    resp.status = WireStatus::Failed;
    peer_.sendMessage(resp);
    slot.abortRecovery();
    return ret;
  }
  slot.resetProbe(msg.generation);
  slot.state = QpSlotState::ReadyPending;
  ret = postEmptyRecv(&slot.shadow, makeInternalWrId(InternalWrKind::RecoveryRecv, msg.generation));
  if (ret != ncclSuccess) {
    WireMsg resp = makeMessage(WireMsgType::RecoveryResp);
    resp.planeId = msg.planeId;
    resp.generation = msg.generation;
    resp.status = WireStatus::Failed;
    peer_.sendMessage(resp);
    slot.abortRecovery();
    return ret;
  }

  WireMsg resp = makeMessage(WireMsgType::RecoveryResp);
  resp.planeId = msg.planeId;
  resp.generation = msg.generation;
  resp.nQps = 1;
  resp.qps[0] = endpointInfo(slot.shadow);
  ret = peer_.sendMessage(resp);
  return ret;
}

ncclResult_t DpfrComm::handleRecoveryResp(const WireMsg& msg) {
  return ncclSuccess;
}

ncclResult_t DpfrComm::handleRecoveryReady(const WireMsg& msg) {
  int slotIndex = findSlotByPlane(slots_, static_cast<int>(msg.planeId));
  WireMsg ack = makeMessage(WireMsgType::RecoveryAck);
  ack.planeId = msg.planeId;
  ack.generation = msg.generation;
  if (slotIndex < 0) {
    ack.status = WireStatus::Failed;
    peer_.sendMessage(ack);
    return ncclRemoteError;
  }
  QpSlot& slot = slots_[slotIndex];
  PlaneSnapshot plane;
  if (!context_->planeTable().get(slot.planeId, &plane) ||
      !planeAllowsQpRecovery(plane) || plane.generation != msg.generation ||
      slot.shadow.generation != msg.generation) {
    ack.status = WireStatus::Retry;
    peer_.sendMessage(ack);
    return ncclSuccess;
  }
  ncclResult_t ret = waitForInternalProbe(&slot, InternalWrKind::RecoveryRecv, msg.generation, kHandshakeTimeoutMs);
  if (ret != ncclSuccess) {
    ack.status = WireStatus::Retry;
    peer_.sendMessage(ack);
    slot.abortRecovery();
    return ncclSuccess;
  }
  ret = commitShadow(slot.planeId, msg.generation);
  if (ret != ncclSuccess) {
    ack.status = WireStatus::Failed;
    peer_.sendMessage(ack);
    return ret;
  }
  if (!context_->planeTable().markHealthyIfGeneration(slot.planeId, msg.generation, "qp-recovery", &plane)) {
    ack.status = WireStatus::Retry;
    peer_.sendMessage(ack);
    slot.quarantine();
    return ncclSuccess;
  }
  ack.status = WireStatus::Ok;
  return peer_.sendMessage(ack);
}

ncclResult_t DpfrComm::commitShadow(int planeId, uint32_t generation) {
  int slotIndex = findSlotByPlane(slots_, planeId);
  if (slotIndex < 0) return ncclInternalError;
  QpSlot& slot = slots_[slotIndex];
  if (slot.shadow.qp == nullptr || !slot.shadow.ready || slot.shadow.generation != generation) return ncclInternalError;
  if (slot.probeGeneration != generation || slot.probeFailed || (!slot.probeSendDone && !slot.probeRecvDone)) return ncclInternalError;
  slot.retireActive();
  slot.active = slot.shadow;
  slot.shadow = QpEndpoint();
  slot.clearProbe();
  slot.state = QpSlotState::Active;
  destroyRetiredIfUnused(&slot);
  DPFR_LOG_INFO("DPFR_IB committed replacement QP plane=%d gen=%u", planeId, generation);
  return ncclSuccess;
}

void DpfrComm::destroyRetiredIfUnused(QpSlot* slot) {
  if (slot == nullptr || slot->retired.empty()) return;
  for (auto it = slot->retired.begin(); it != slot->retired.end();) {
    bool inUse = false;
    for (const DpfrRequest& req : requests_) {
      if ((req.state == RequestState::Posted || req.state == RequestState::NeedsQuery || req.state == RequestState::NeedsReplay) &&
          req.planeId == slot->planeId && req.generation == it->generation) {
        inUse = true;
        break;
      }
    }
    if (inUse) {
      ++it;
    } else {
      DPFR_LOG_INFO("DPFR_IB destroying retired QP plane=%d gen=%u", slot->planeId, it->generation);
      destroyEndpoint(&*it);
      it = slot->retired.erase(it);
    }
  }
}

void DpfrComm::onPlaneBroken(int planeId, uint32_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (QpSlot& slot : slots_) {
    if (slot.planeId != planeId) continue;
    if (slot.shadow.qp != nullptr && slot.shadow.generation <= generation) slot.abortRecovery();
    slot.quarantine();
    for (DpfrRequest& req : requests_) {
      if (req.planeId == planeId && req.state == RequestState::Posted) {
        req.state = req.kind == RequestKind::Send ? RequestState::NeedsQuery : RequestState::NeedsReplay;
      }
    }
  }
}

void DpfrComm::onPlaneRecovering(int planeId, uint32_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (QpSlot& slot : slots_) {
    if (slot.planeId != planeId) continue;
    if (slot.shadow.qp != nullptr && slot.shadow.generation != generation) slot.abortRecovery();
    if (slot.active.generation != generation || slot.state != QpSlotState::Active) slot.state = QpSlotState::Rebuilding;
  }
}

void DpfrComm::markRecvDone(uint64_t seq) {
  recvJournal_.insert(seq);
  recvJournalOrder_.push_back(seq);
  while (recvJournalOrder_.size() > 4096) {
    recvJournal_.erase(recvJournalOrder_.front());
    recvJournalOrder_.pop_front();
  }
}

bool DpfrComm::recvDone(uint64_t seq) const {
  return recvJournal_.find(seq) != recvJournal_.end();
}

void DpfrComm::fail() {
  failed_ = true;
}

void DpfrComm::close() {
  if (closed_) return;
  closed_ = true;
  if (context_) context_->unregisterComm(this);
  peer_.close();
  for (QpSlot& slot : slots_) slot.markDead();
  slots_.clear();
}

ncclResult_t DpfrNet::init(void** ctx, uint64_t commId, ncclNetCommConfig_t* config, ncclDebugLogger_t logger, ncclProfilerCallback_t profiler) {
  DpfrContext& context = globalContext();
  ncclResult_t ret = context.init(commId, logger);
  if (ctx) *ctx = &context;
  return ret;
}

ncclResult_t DpfrNet::devices(int* ndev) {
  if (ndev == nullptr) return ncclInvalidArgument;
  *ndev = globalContext().initialized() && !globalContext().devices().empty() ? 1 : 0;
  return ncclSuccess;
}

ncclResult_t DpfrNet::getProperties(int dev, ncclNetProperties_t* props) {
  if (dev != 0 || props == nullptr) return ncclInvalidArgument;
  memset(props, 0, sizeof(*props));
  props->name = const_cast<char*>("DPFR_IB");
  props->pciPath = nullptr;
  props->guid = 0;
  props->ptrSupport = NCCL_PTR_HOST | NCCL_PTR_CUDA;
  props->regIsGlobal = 0;
  props->forceFlush = 0;
  props->speed = 100000 * static_cast<int>(std::max<size_t>(1, globalContext().devices().size()));
  props->port = 0;
  props->latency = 0;
  props->maxComms = 1024 * 1024;
  props->maxRecvs = 1;
  props->netDeviceType = NCCL_NET_DEVICE_HOST;
  props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
  props->vProps.ndevs = 1;
  props->vProps.devs[0] = 0;
  props->maxP2pBytes = kMaxPluginBytes;
  props->maxCollBytes = kMaxPluginBytes;
  props->maxMultiRequestSize = 1;
  return ncclSuccess;
}

ncclResult_t DpfrNet::listen(void* ctx, int dev, void* handle, void** listenComm) {
  if (dev != 0 || handle == nullptr || listenComm == nullptr) return ncclInvalidArgument;
  *listenComm = nullptr;
  DpfrContext* context = ctx ? static_cast<DpfrContext*>(ctx) : &globalContext();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return ncclSystemError;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 64) != 0) {
    close(fd);
    return ncclSystemError;
  }
  sockaddr_in bound = {};
  socklen_t len = sizeof(bound);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
    close(fd);
    return ncclSystemError;
  }
  setSocketNonBlocking(fd, true);

  DpfrListenComm* listenObj = new DpfrListenComm();
  listenObj->context = context;
  listenObj->fd = fd;
  std::string addrText = context->config().controlAddr.empty() ? firstNonLoopbackIpv4() : context->config().controlAddr;
  inet_pton(AF_INET, addrText.c_str(), &listenObj->handle.ipv4);
  listenObj->handle.port = ntohs(bound.sin_port);
  listenObj->handle.nonce = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(listenObj));
  memcpy(handle, &listenObj->handle, sizeof(listenObj->handle));
  *listenComm = listenObj;
  return ncclSuccess;
}

ncclResult_t DpfrNet::connect(void* ctx, int dev, void* handle, void** sendComm, ncclNetDeviceHandle_t** sendDevComm) {
  if (sendDevComm) *sendDevComm = nullptr;
  if (sendComm == nullptr || handle == nullptr || dev != 0) return ncclInvalidArgument;
  *sendComm = nullptr;
  DpfrNetHandle netHandle;
  memcpy(&netHandle, handle, sizeof(netHandle));
  if (!validHandle(netHandle)) return ncclInvalidArgument;
  DpfrContext* context = ctx ? static_cast<DpfrContext*>(ctx) : &globalContext();
  DpfrComm* comm = new DpfrComm(context, true);
  ncclResult_t ret = comm->initAsConnector(netHandle);
  if (ret != ncclSuccess) {
    delete comm;
    return ret;
  }
  *sendComm = comm;
  return ncclSuccess;
}

ncclResult_t DpfrNet::accept(void* listenComm, void** recvComm, ncclNetDeviceHandle_t** recvDevComm) {
  if (recvDevComm) *recvDevComm = nullptr;
  if (listenComm == nullptr || recvComm == nullptr) return ncclInvalidArgument;
  *recvComm = nullptr;
  DpfrListenComm* listenObj = static_cast<DpfrListenComm*>(listenComm);
  int fd = ::accept(listenObj->fd, nullptr, nullptr);
  if (fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return ncclSuccess;
    return ncclSystemError;
  }
  setSocketNonBlocking(fd, false);
  WireMsg initReq;
  ncclResult_t ret = recvAll(fd, &initReq, sizeof(initReq), kHandshakeTimeoutMs);
  if (ret != ncclSuccess || initReq.type != WireMsgType::InitReq || !validMessage(initReq)) {
    close(fd);
    return ret == ncclSuccess ? ncclRemoteError : ret;
  }
  DpfrComm* comm = new DpfrComm(listenObj->context, false);
  ret = comm->initFromAcceptedFd(fd, &initReq);
  if (ret != ncclSuccess) {
    delete comm;
    return ret;
  }
  *recvComm = comm;
  return ncclSuccess;
}

ncclResult_t DpfrNet::regMr(void* comm, void* data, size_t size, int type, void** mhandle) {
  return registerMr(&globalContext().devices(), data, size, type, mhandle);
}

ncclResult_t DpfrNet::regMrDmaBuf(void* comm, void* data, size_t size, int type, uint64_t offset, int fd, void** mhandle) {
  DPFR_LOG_WARN("DPFR_IB DMABUF registration is not supported in V1");
  return ncclInvalidUsage;
}

ncclResult_t DpfrNet::deregMr(void* comm, void* mhandle) {
  return deregisterMr(mhandle);
}

ncclResult_t DpfrNet::isend(void* sendComm, void* data, size_t size, int tag, void* mhandle, void* phandle, void** request) {
  return static_cast<DpfrComm*>(sendComm)->isend(data, size, tag, mhandle, request);
}

ncclResult_t DpfrNet::irecv(void* recvComm, int n, void** data, size_t* sizes, int* tags, void** mhandles, void** phandles, void** request) {
  return static_cast<DpfrComm*>(recvComm)->irecv(n, data, sizes, tags, mhandles, request);
}

ncclResult_t DpfrNet::iflush(void* recvComm, int n, void** data, int* sizes, void** mhandles, void** request) {
  return static_cast<DpfrComm*>(recvComm)->iflush(n, data, sizes, mhandles, request);
}

ncclResult_t DpfrNet::test(void* request, int* done, int* size) {
  if (request == nullptr) return ncclInvalidArgument;
  DpfrRequest* req = static_cast<DpfrRequest*>(request);
  return static_cast<DpfrComm*>(req->owner)->test(req, done, size);
}

ncclResult_t DpfrNet::closeSend(void* sendComm) {
  delete static_cast<DpfrComm*>(sendComm);
  return ncclSuccess;
}

ncclResult_t DpfrNet::closeRecv(void* recvComm) {
  delete static_cast<DpfrComm*>(recvComm);
  return ncclSuccess;
}

ncclResult_t DpfrNet::closeListen(void* listenComm) {
  DpfrListenComm* listenObj = static_cast<DpfrListenComm*>(listenComm);
  if (listenObj) {
    if (listenObj->fd >= 0) close(listenObj->fd);
    delete listenObj;
  }
  return ncclSuccess;
}

ncclResult_t DpfrNet::getDeviceMr(void* comm, void* mhandle, void** dptrMhandle) {
  if (dptrMhandle) *dptrMhandle = nullptr;
  return ncclInvalidUsage;
}

ncclResult_t DpfrNet::irecvConsumed(void* recvComm, int n, void* request) {
  return ncclSuccess;
}

ncclResult_t DpfrNet::makeVDevice(int* d, ncclNetVDeviceProps_t* props) {
  if (d) *d = 0;
  return ncclSuccess;
}

ncclResult_t DpfrNet::finalize(void* ctx) {
  return globalContext().finalize();
}

ncclResult_t DpfrNet::setNetAttr(void* ctx, ncclNetAttr_v11_t* netAttr) {
  return ncclSuccess;
}

}  // namespace dpfr
