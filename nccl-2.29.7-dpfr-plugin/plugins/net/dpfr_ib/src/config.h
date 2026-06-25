#ifndef DPFR_IB_CONFIG_H_
#define DPFR_IB_CONFIG_H_

#include "net.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dpfr {

constexpr int kDefaultDpfrUdpPort = 19767;
constexpr int kDefaultGidIndex = 0;
constexpr int kMaxPlanes = 8;

struct PlaneConfig {
  std::string hcaName;
  int port = 1;
  int planeId = 0;
  std::string probeIp;
  std::string probeIface;
  std::string dpfrIface;
};

struct PluginConfig {
  std::vector<PlaneConfig> planes;
  bool autoDiscover = false;
  int gidIndex = kDefaultGidIndex;
  std::string controlAddr;
  std::string controlTcpServer;
  int controlTcpPort = 0;
  int dpfrUdpPort = kDefaultDpfrUdpPort;
};

ncclResult_t loadConfig(PluginConfig* config);
ncclResult_t parseHcaList(const char* value, std::vector<PlaneConfig>* planes, std::string* error);
bool parsePlaneControlLine(const std::string& line, int* planeId, bool* recovered);

}  // namespace dpfr

#endif  // DPFR_IB_CONFIG_H_
