#ifndef DPFR_IB_MR_H_
#define DPFR_IB_MR_H_

#include "verbs_device.h"

#include "net.h"

#include <infiniband/verbs.h>

#include <cstddef>
#include <vector>

namespace dpfr {

struct DpfrMr {
  void* base = nullptr;
  size_t size = 0;
  int type = 0;
  std::vector<ibv_mr*> planeMrs;
};

ncclResult_t registerMr(std::vector<PlaneDevice>* devices, void* data, size_t size, int type, void** mhandle);
ncclResult_t deregisterMr(void* mhandle);
ibv_mr* mrForPlane(void* mhandle, const std::vector<PlaneDevice>& devices, int planeId);

}  // namespace dpfr

#endif  // DPFR_IB_MR_H_
