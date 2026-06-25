#ifndef DPFR_IB_VERBS_DEVICE_H_
#define DPFR_IB_VERBS_DEVICE_H_

#include "config.h"
#include "control_wire.h"

#include <infiniband/verbs.h>

#include <string>
#include <vector>

namespace dpfr {

struct PlaneDevice {
  int planeId = 0;
  std::string hcaName;
  int port = 1;
  int gidIndex = 0;
  ibv_context* context = nullptr;
  ibv_pd* pd = nullptr;
  ibv_port_attr portAttr = {};
  ibv_gid gid = {};
  uint8_t linkLayer = 0;
};

ncclResult_t openPlaneDevices(const PluginConfig& config, std::vector<PlaneDevice>* devices);
void closePlaneDevices(std::vector<PlaneDevice>* devices);
ncclResult_t refreshPlaneDevice(PlaneDevice* device);
bool isZeroGid(const ibv_gid& gid);
PlaneDevice* findPlaneDevice(std::vector<PlaneDevice>* devices, int planeId);
const PlaneDevice* findPlaneDevice(const std::vector<PlaneDevice>* devices, int planeId);
std::string firstNonLoopbackIpv4();

}  // namespace dpfr

#endif  // DPFR_IB_VERBS_DEVICE_H_
