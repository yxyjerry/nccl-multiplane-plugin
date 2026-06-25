#ifndef DPFR_IB_MONITOR_H_
#define DPFR_IB_MONITOR_H_

#include "config.h"

#include <atomic>
#include <thread>

namespace dpfr {

class DpfrContext;

class Monitor {
 public:
  explicit Monitor(DpfrContext* context);
  ~Monitor();

  void start(const PluginConfig& config);
  void stop();

 private:
  void tcpThreadMain(PluginConfig config);
  void udpThreadMain(PluginConfig config);
  void probeThreadMain(PluginConfig config);

  DpfrContext* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::thread tcpThread_;
  std::thread udpThread_;
  std::thread probeThread_;
};

}  // namespace dpfr

#endif  // DPFR_IB_MONITOR_H_
