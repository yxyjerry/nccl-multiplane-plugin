#ifndef DPFR_IB_COMM_H_
#define DPFR_IB_COMM_H_

#include "context.h"
#include "peer_control.h"
#include "qp_selector.h"
#include "request.h"
#include "replay_engine.h"

#include "net.h"

#include <array>
#include <deque>
#include <mutex>
#include <set>
#include <vector>

namespace dpfr {

class DpfrListenComm {
 public:
  DpfrContext* context = nullptr;
  int fd = -1;
  DpfrNetHandle handle;
};

class DpfrComm {
 public:
  explicit DpfrComm(DpfrContext* context, bool sendSide);
  ~DpfrComm();

  DpfrComm(const DpfrComm&) = delete;
  DpfrComm& operator=(const DpfrComm&) = delete;

  ncclResult_t initFromAcceptedFd(int fd, WireMsg* initReq);
  ncclResult_t initAsConnector(const DpfrNetHandle& handle);
  ncclResult_t isend(void* data, size_t size, int tag, void* mhandle, void** request);
  ncclResult_t irecv(int n, void** data, size_t* sizes, int* tags, void** mhandles, void** request);
  ncclResult_t iflush(int n, void** data, int* sizes, void** mhandles, void** request);
  ncclResult_t test(DpfrRequest* request, int* done, int* size);
  void close();

  ncclResult_t progress();
  ncclResult_t handleControl(const WireMsg& msg);
  ncclResult_t sendPlaneEvent(int planeId, uint32_t generation, bool recovered);
  ncclResult_t queryRemote(uint64_t seq, bool* done, bool* retry);
  ncclResult_t postRequest(DpfrRequest* req);
  ncclResult_t repostRecvForSeq(uint64_t seq, bool* reposted);
  void onPlaneBroken(int planeId, uint32_t generation);
  void onPlaneRecovering(int planeId, uint32_t generation);
  void markRecvDone(uint64_t seq);
  bool recvDone(uint64_t seq) const;

  DpfrContext* context() { return context_; }
  PeerControl& peer() { return peer_; }
  std::vector<QpSlot>& slots() { return slots_; }
  const std::vector<QpSlot>& slots() const { return slots_; }
  bool sendSide() const { return sendSide_; }
  bool failed() const { return failed_; }
  void fail();

 private:
  friend class ReplayEngine;

  DpfrRequest* allocRequest();
  bool hasOutstandingLocked() const;
  void releaseDoneRequestsLocked();
  ncclResult_t createInitialSlots();
  ncclResult_t exportQps(WireMsgType type, WireMsg* msg) const;
  ncclResult_t importRemoteQps(const WireMsg& msg);
  ncclResult_t progressCqs();
  ncclResult_t progressControl();
  ncclResult_t progressRecovery();
  ncclResult_t ensureRecoveryShadow(QpSlot* slot, uint32_t generation);
  ncclResult_t handleRecoveryReq(const WireMsg& msg);
  ncclResult_t handleRecoveryResp(const WireMsg& msg);
  ncclResult_t handleRecoveryReady(const WireMsg& msg);
  ncclResult_t handleQuery(const WireMsg& msg);
  ncclResult_t commitShadow(int planeId, uint32_t generation);
  void destroyRetiredIfUnused(QpSlot* slot);
  ncclResult_t markLocalPlaneBrokenFromCq(QpSlot* slot, uint32_t generation, ibv_wc_status status);
  void recordInternalCqe(QpSlot* slot, uint64_t wrId, ibv_wc_status status);
  ncclResult_t waitForInternalProbe(QpSlot* slot, InternalWrKind kind, uint32_t generation, int timeoutMs);

  DpfrContext* context_ = nullptr;
  bool sendSide_ = false;
  bool closed_ = false;
  bool failed_ = false;
  PeerControl peer_;
  std::vector<QpSlot> slots_;
  std::array<DpfrRequest, NCCL_NET_MAX_REQUESTS> requests_;
  uint64_t nextSeq_ = 1;
  QpSelector selector_;
  ReplayEngine replay_;
  std::set<uint64_t> recvJournal_;
  std::deque<uint64_t> recvJournalOrder_;
  mutable std::mutex mutex_;
};

class DpfrNet {
 public:
  static ncclResult_t init(void** ctx, uint64_t commId, ncclNetCommConfig_t* config, ncclDebugLogger_t logger, ncclProfilerCallback_t profiler);
  static ncclResult_t devices(int* ndev);
  static ncclResult_t getProperties(int dev, ncclNetProperties_t* props);
  static ncclResult_t listen(void* ctx, int dev, void* handle, void** listenComm);
  static ncclResult_t connect(void* ctx, int dev, void* handle, void** sendComm, ncclNetDeviceHandle_t** sendDevComm);
  static ncclResult_t accept(void* listenComm, void** recvComm, ncclNetDeviceHandle_t** recvDevComm);
  static ncclResult_t regMr(void* comm, void* data, size_t size, int type, void** mhandle);
  static ncclResult_t regMrDmaBuf(void* comm, void* data, size_t size, int type, uint64_t offset, int fd, void** mhandle);
  static ncclResult_t deregMr(void* comm, void* mhandle);
  static ncclResult_t isend(void* sendComm, void* data, size_t size, int tag, void* mhandle, void* phandle, void** request);
  static ncclResult_t irecv(void* recvComm, int n, void** data, size_t* sizes, int* tags, void** mhandles, void** phandles, void** request);
  static ncclResult_t iflush(void* recvComm, int n, void** data, int* sizes, void** mhandles, void** request);
  static ncclResult_t test(void* request, int* done, int* size);
  static ncclResult_t closeSend(void* sendComm);
  static ncclResult_t closeRecv(void* recvComm);
  static ncclResult_t closeListen(void* listenComm);
  static ncclResult_t getDeviceMr(void* comm, void* mhandle, void** dptrMhandle);
  static ncclResult_t irecvConsumed(void* recvComm, int n, void* request);
  static ncclResult_t makeVDevice(int* d, ncclNetVDeviceProps_t* props);
  static ncclResult_t finalize(void* ctx);
  static ncclResult_t setNetAttr(void* ctx, ncclNetAttr_v11_t* netAttr);
};

}  // namespace dpfr

#endif  // DPFR_IB_COMM_H_
