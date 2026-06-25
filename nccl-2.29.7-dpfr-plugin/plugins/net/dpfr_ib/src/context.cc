#include "context.h"

#include "comm.h"
#include "log.h"
#include "monitor.h"

#include <algorithm>

namespace dpfr {

DpfrContext& globalContext() {
  static DpfrContext context;
  return context;
}

ncclResult_t DpfrContext::init(uint64_t commId, ncclDebugLogger_t logger) {
  setLogger(logger);
  if (initialized_) return ncclSuccess;

  ncclResult_t ret = loadConfig(&config_);
  if (ret != ncclSuccess) return ret;

  ret = openPlaneDevices(config_, &devices_);
  if (ret != ncclSuccess) return ret;

  std::vector<int> planeIds;
  for (const PlaneDevice& device : devices_) planeIds.push_back(device.planeId);
  planeTable_.reset(planeIds);

  monitor_ = new Monitor(this);
  monitor_->start(config_);
  initialized_ = true;
  DPFR_LOG_INFO("DPFR_IB initialized commId=%llu planes=%zu", static_cast<unsigned long long>(commId), devices_.size());
  return ncclSuccess;
}

ncclResult_t DpfrContext::finalize() {
  if (monitor_) {
    monitor_->stop();
    delete monitor_;
    monitor_ = nullptr;
  }
  closePlaneDevices(&devices_);
  initialized_ = false;
  return ncclSuccess;
}

void DpfrContext::registerComm(DpfrComm* comm) {
  std::lock_guard<std::mutex> lock(commMutex_);
  comms_.push_back(comm);
}

void DpfrContext::unregisterComm(DpfrComm* comm) {
  std::lock_guard<std::mutex> lock(commMutex_);
  comms_.erase(std::remove(comms_.begin(), comms_.end(), comm), comms_.end());
}

void DpfrContext::applyPlaneEvent(int planeId, bool recovered, const char* source, bool broadcast) {
  PlaneSnapshot snapshot;
  bool ok = recovered ? planeTable_.markRecovering(planeId, source, &snapshot)
                      : planeTable_.markBroken(planeId, source, &snapshot);
  if (!ok) {
    DPFR_LOG_WARN("DPFR_IB ignored event for unknown plane %d", planeId);
    return;
  }

  std::vector<DpfrComm*> comms;
  {
    std::lock_guard<std::mutex> lock(commMutex_);
    comms = comms_;
  }

  DPFR_LOG_WARN("DPFR_IB plane %d -> %s gen=%u source=%s", planeId,
                planeHealthName(snapshot.health), snapshot.generation, snapshot.source.c_str());
  for (DpfrComm* comm : comms) {
    if (recovered) comm->onPlaneRecovering(planeId, snapshot.generation);
    else comm->onPlaneBroken(planeId, snapshot.generation);
    if (broadcast) comm->sendPlaneEvent(planeId, snapshot.generation, recovered);
  }
}

}  // namespace dpfr
