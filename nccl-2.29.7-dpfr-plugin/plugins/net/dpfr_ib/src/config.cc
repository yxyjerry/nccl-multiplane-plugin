#include "config.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dpfr {
namespace {

std::string trim(const std::string& in) {
  size_t first = 0;
  while (first < in.size() && std::isspace(static_cast<unsigned char>(in[first]))) first++;
  size_t last = in.size();
  while (last > first && std::isspace(static_cast<unsigned char>(in[last - 1]))) last--;
  return in.substr(first, last - first);
}

std::string lower(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return v;
}

int envInt(const char* name, int defaultValue) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return defaultValue;
  char* end = nullptr;
  long parsed = std::strtol(value, &end, 0);
  if (end == value) return defaultValue;
  return static_cast<int>(parsed);
}

std::string envStr(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

ncclResult_t parseHcaList(const char* value, std::vector<PlaneConfig>* planes, std::string* error) {
  planes->clear();
  if (value == nullptr || value[0] == '\0') return ncclSuccess;

  std::stringstream entries(value);
  std::string entry;
  while (std::getline(entries, entry, ',')) {
    entry = trim(entry);
    if (entry.empty()) continue;

    std::stringstream fields(entry);
    std::string hca;
    std::string portText;
    std::string planeText;
    if (!std::getline(fields, hca, ':') || !std::getline(fields, portText, ':') || !std::getline(fields, planeText, ':')) {
      if (error) *error = "expected hca:port:plane in NCCL_DPFR_IB_HCA_LIST entry '" + entry + "'";
      return ncclInvalidArgument;
    }

    char* end = nullptr;
    long port = std::strtol(portText.c_str(), &end, 0);
    if (end == portText.c_str() || port <= 0) {
      if (error) *error = "invalid port in NCCL_DPFR_IB_HCA_LIST entry '" + entry + "'";
      return ncclInvalidArgument;
    }
    end = nullptr;
    long planeId = std::strtol(planeText.c_str(), &end, 0);
    if (end == planeText.c_str() || planeId < 0 || planeId >= kMaxPlanes) {
      if (error) *error = "invalid plane id in NCCL_DPFR_IB_HCA_LIST entry '" + entry + "'";
      return ncclInvalidArgument;
    }

    PlaneConfig plane;
    plane.hcaName = trim(hca);
    plane.port = static_cast<int>(port);
    plane.planeId = static_cast<int>(planeId);
    planes->push_back(plane);
  }

  return ncclSuccess;
}

bool parsePlaneControlLine(const std::string& line, int* planeId, bool* recovered) {
  std::string normalized = lower(line);
  for (char& c : normalized) {
    if (c == '=' || c == ',' || c == ';' || c == ':') c = ' ';
  }

  std::stringstream ss(normalized);
  std::string token;
  bool sawPlane = false;
  bool sawState = false;
  while (ss >> token) {
    if (token == "plane") {
      int id = -1;
      if (ss >> id) {
        *planeId = id;
        sawPlane = true;
      }
      continue;
    }
    if (token == "broken" || token == "down" || token == "fail" || token == "failed") {
      *recovered = false;
      sawState = true;
      continue;
    }
    if (token == "recovered" || token == "healthy" || token == "up" || token == "recover") {
      *recovered = true;
      sawState = true;
      continue;
    }
  }
  return sawPlane && sawState && *planeId >= 0;
}

ncclResult_t loadConfig(PluginConfig* config) {
  *config = PluginConfig();
  config->gidIndex = envInt("NCCL_DPFR_IB_GID_INDEX", kDefaultGidIndex);
  config->controlAddr = envStr("NCCL_DPFR_IB_CONTROL_ADDR");
  config->controlTcpServer = envStr("NCCL_DPFR_IB_CONTROL_TCP_SERVER");
  config->controlTcpPort = envInt("NCCL_DPFR_IB_CONTROL_TCP_PORT", 0);
  config->dpfrUdpPort = envInt("NCCL_DPFR_IB_DPFR_UDP_PORT", kDefaultDpfrUdpPort);

  std::string error;
  ncclResult_t ret = parseHcaList(std::getenv("NCCL_DPFR_IB_HCA_LIST"), &config->planes, &error);
  if (ret != ncclSuccess) {
    DPFR_LOG_WARN("DPFR_IB config error: %s", error.c_str());
    return ret;
  }
  config->autoDiscover = config->planes.empty();

  for (PlaneConfig& plane : config->planes) {
    char name[128];
    snprintf(name, sizeof(name), "NCCL_DPFR_IB_PLANE%d_PROBE_IP", plane.planeId);
    plane.probeIp = envStr(name);
    snprintf(name, sizeof(name), "NCCL_DPFR_IB_PLANE%d_PROBE_IFACE", plane.planeId);
    plane.probeIface = envStr(name);
    snprintf(name, sizeof(name), "NCCL_DPFR_IB_PLANE%d_DPFR_IFACE", plane.planeId);
    plane.dpfrIface = envStr(name);
  }

  return ncclSuccess;
}

}  // namespace dpfr
