#ifndef DPFR_IB_CONTEXT_H_
#define DPFR_IB_CONTEXT_H_

#include "config.h"
#include "plane_state.h"
#include "verbs_device.h"

#include "net.h"

#include <mutex>
#include <vector>

namespace dpfr {

class DpfrComm;
class Monitor;

class DpfrContext {
 public:
  ncclResult_t init(uint64_t commId, ncclDebugLogger_t logger);
  ncclResult_t finalize();
  bool initialized() const { return initialized_; }

  PluginConfig& config() { return config_; }
  const PluginConfig& config() const { return config_; }
  PlaneTable& planeTable() { return planeTable_; }
  const PlaneTable& planeTable() const { return planeTable_; }
  std::vector<PlaneDevice>& devices() { return devices_; }
  const std::vector<PlaneDevice>& devices() const { return devices_; }

  void registerComm(DpfrComm* comm);
  void unregisterComm(DpfrComm* comm);
  void applyPlaneEvent(int planeId, bool recovered, const char* source, bool broadcast);

 private:
  PluginConfig config_;
  PlaneTable planeTable_;
  std::vector<PlaneDevice> devices_;
  std::vector<DpfrComm*> comms_;
  std::mutex commMutex_;
  Monitor* monitor_ = nullptr;
  bool initialized_ = false;
};

DpfrContext& globalContext();

}  // namespace dpfr

#endif  // DPFR_IB_CONTEXT_H_
