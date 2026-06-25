#include "verbs_device.h"

#include "log.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <set>

namespace dpfr {
namespace {

bool nameMatches(ibv_device* device, const std::string& hcaName) {
  return hcaName == ibv_get_device_name(device);
}

ncclResult_t openOnePlane(ibv_device* device, const PlaneConfig& cfg, int gidIndex, std::vector<PlaneDevice>* devices) {
  PlaneDevice plane;
  plane.planeId = cfg.planeId;
  plane.hcaName = ibv_get_device_name(device);
  plane.port = cfg.port;
  plane.gidIndex = gidIndex;
  plane.context = ibv_open_device(device);
  if (plane.context == nullptr) {
    DPFR_LOG_WARN("DPFR_IB failed to open HCA %s", plane.hcaName.c_str());
    return ncclSystemError;
  }
  if (ibv_query_port(plane.context, plane.port, &plane.portAttr) != 0 || plane.portAttr.state != IBV_PORT_ACTIVE) {
    DPFR_LOG_WARN("DPFR_IB HCA %s port %d is not active", plane.hcaName.c_str(), plane.port);
    ibv_close_device(plane.context);
    return ncclSystemError;
  }
  if (ibv_query_gid(plane.context, plane.port, gidIndex, &plane.gid) != 0) {
    DPFR_LOG_WARN("DPFR_IB failed to query gid index %d on %s:%d", gidIndex, plane.hcaName.c_str(), plane.port);
    ibv_close_device(plane.context);
    return ncclSystemError;
  }
  plane.pd = ibv_alloc_pd(plane.context);
  if (plane.pd == nullptr) {
    DPFR_LOG_WARN("DPFR_IB failed to allocate PD on %s:%d", plane.hcaName.c_str(), plane.port);
    ibv_close_device(plane.context);
    return ncclSystemError;
  }
  plane.linkLayer = plane.portAttr.link_layer;
  devices->push_back(plane);
  DPFR_LOG_INFO("DPFR_IB opened plane %d on %s:%d gidIndex=%d", plane.planeId, plane.hcaName.c_str(), plane.port, gidIndex);
  return ncclSuccess;
}

}  // namespace

bool isZeroGid(const ibv_gid& gid) {
  static const uint8_t zeros[16] = {};
  return memcmp(&gid, zeros, sizeof(zeros)) == 0;
}

ncclResult_t refreshPlaneDevice(PlaneDevice* device) {
  if (device == nullptr || device->context == nullptr || device->pd == nullptr) {
    DPFR_LOG_WARN("DPFR_IB cannot refresh plane metadata: invalid context or PD");
    return ncclSystemError;
  }

  ibv_port_attr portAttr = {};
  if (ibv_query_port(device->context, device->port, &portAttr) != 0) {
    DPFR_LOG_WARN("DPFR_IB refresh failed: ibv_query_port failed for plane %d %s:%d",
                  device->planeId, device->hcaName.c_str(), device->port);
    return ncclInvalidUsage;
  }
  if (portAttr.state != IBV_PORT_ACTIVE) {
    DPFR_LOG_WARN("DPFR_IB refresh not ready: plane %d %s:%d port state=%d",
                  device->planeId, device->hcaName.c_str(), device->port, portAttr.state);
    return ncclInvalidUsage;
  }

  ibv_gid gid = {};
  if (ibv_query_gid(device->context, device->port, device->gidIndex, &gid) != 0) {
    DPFR_LOG_WARN("DPFR_IB refresh failed: ibv_query_gid failed for plane %d %s:%d gidIndex=%d",
                  device->planeId, device->hcaName.c_str(), device->port, device->gidIndex);
    return ncclInvalidUsage;
  }
  if (isZeroGid(gid)) {
    DPFR_LOG_WARN("DPFR_IB refresh not ready: plane %d %s:%d gidIndex=%d is zero",
                  device->planeId, device->hcaName.c_str(), device->port, device->gidIndex);
    return ncclInvalidUsage;
  }

  device->portAttr = portAttr;
  device->gid = gid;
  device->linkLayer = portAttr.link_layer;
  DPFR_LOG_INFO("DPFR_IB refreshed plane %d %s:%d mtu=%d lid=%u linkLayer=%u gidIndex=%d",
                device->planeId, device->hcaName.c_str(), device->port,
                portAttr.active_mtu, portAttr.lid, device->linkLayer, device->gidIndex);
  return ncclSuccess;
}

ncclResult_t openPlaneDevices(const PluginConfig& config, std::vector<PlaneDevice>* devices) {
  devices->clear();
  int nDevices = 0;
  ibv_device** list = ibv_get_device_list(&nDevices);
  if (list == nullptr || nDevices == 0) {
    DPFR_LOG_WARN("DPFR_IB found no ibverbs devices");
    if (list) ibv_free_device_list(list);
    return ncclSystemError;
  }

  ncclResult_t result = ncclSuccess;
  if (config.autoDiscover) {
    int nextPlane = 0;
    for (int d = 0; d < nDevices && nextPlane < kMaxPlanes; ++d) {
      ibv_context* ctx = ibv_open_device(list[d]);
      if (ctx == nullptr) continue;
      ibv_device_attr attr = {};
      if (ibv_query_device(ctx, &attr) != 0) {
        ibv_close_device(ctx);
        continue;
      }
      for (int port = 1; port <= attr.phys_port_cnt && nextPlane < kMaxPlanes; ++port) {
        ibv_port_attr portAttr = {};
        if (ibv_query_port(ctx, port, &portAttr) != 0 || portAttr.state != IBV_PORT_ACTIVE) continue;
        PlaneConfig cfg;
        cfg.hcaName = ibv_get_device_name(list[d]);
        cfg.port = port;
        cfg.planeId = nextPlane++;
        result = openOnePlane(list[d], cfg, config.gidIndex, devices);
        if (result != ncclSuccess) break;
      }
      ibv_close_device(ctx);
      if (result != ncclSuccess) break;
    }
  } else {
    std::set<int> seenPlanes;
    for (const PlaneConfig& cfg : config.planes) {
      if (!seenPlanes.insert(cfg.planeId).second) {
        DPFR_LOG_WARN("DPFR_IB duplicate plane id %d in HCA list", cfg.planeId);
        result = ncclInvalidArgument;
        break;
      }
      ibv_device* match = nullptr;
      for (int d = 0; d < nDevices; ++d) {
        if (nameMatches(list[d], cfg.hcaName)) {
          match = list[d];
          break;
        }
      }
      if (match == nullptr) {
        DPFR_LOG_WARN("DPFR_IB requested HCA %s not found", cfg.hcaName.c_str());
        result = ncclInvalidArgument;
        break;
      }
      result = openOnePlane(match, cfg, config.gidIndex, devices);
      if (result != ncclSuccess) break;
    }
  }

  ibv_free_device_list(list);
  if (result != ncclSuccess || devices->empty()) {
    closePlaneDevices(devices);
    return result == ncclSuccess ? ncclSystemError : result;
  }
  return ncclSuccess;
}

void closePlaneDevices(std::vector<PlaneDevice>* devices) {
  for (PlaneDevice& device : *devices) {
    if (device.pd) ibv_dealloc_pd(device.pd);
    if (device.context) ibv_close_device(device.context);
    device.pd = nullptr;
    device.context = nullptr;
  }
  devices->clear();
}

PlaneDevice* findPlaneDevice(std::vector<PlaneDevice>* devices, int planeId) {
  for (PlaneDevice& device : *devices) {
    if (device.planeId == planeId) return &device;
  }
  return nullptr;
}

const PlaneDevice* findPlaneDevice(const std::vector<PlaneDevice>* devices, int planeId) {
  for (const PlaneDevice& device : *devices) {
    if (device.planeId == planeId) return &device;
  }
  return nullptr;
}

std::string firstNonLoopbackIpv4() {
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return "127.0.0.1";
  char host[INET_ADDRSTRLEN] = {};
  std::string result = "127.0.0.1";
  for (ifaddrs* it = ifaddr; it != nullptr; it = it->ifa_next) {
    if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
    if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
    if (inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) != nullptr) {
      result = host;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return result;
}

}  // namespace dpfr
