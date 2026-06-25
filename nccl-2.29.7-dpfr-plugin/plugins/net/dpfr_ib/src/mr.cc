#include "mr.h"

#include "log.h"

#include <cstdlib>

namespace dpfr {

ncclResult_t registerMr(std::vector<PlaneDevice>* devices, void* data, size_t size, int type, void** mhandle) {
  if (devices == nullptr || devices->empty() || data == nullptr || size == 0 || mhandle == nullptr) return ncclInvalidArgument;
  DpfrMr* mr = new DpfrMr();
  mr->base = data;
  mr->size = size;
  mr->type = type;
  mr->planeMrs.resize(devices->size(), nullptr);

  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
#ifdef IBV_ACCESS_RELAXED_ORDERING
  access |= IBV_ACCESS_RELAXED_ORDERING;
#endif

  for (size_t i = 0; i < devices->size(); ++i) {
    mr->planeMrs[i] = ibv_reg_mr((*devices)[i].pd, data, size, access);
    if (mr->planeMrs[i] == nullptr) {
      DPFR_LOG_WARN("DPFR_IB failed to register MR on plane %d; CUDA buffers require GPUDirect RDMA support", (*devices)[i].planeId);
      deregisterMr(mr);
      return ncclSystemError;
    }
  }

  *mhandle = mr;
  return ncclSuccess;
}

ncclResult_t deregisterMr(void* mhandle) {
  if (mhandle == nullptr) return ncclSuccess;
  DpfrMr* mr = static_cast<DpfrMr*>(mhandle);
  for (ibv_mr* verbsMr : mr->planeMrs) {
    if (verbsMr) ibv_dereg_mr(verbsMr);
  }
  delete mr;
  return ncclSuccess;
}

ibv_mr* mrForPlane(void* mhandle, const std::vector<PlaneDevice>& devices, int planeId) {
  if (mhandle == nullptr) return nullptr;
  DpfrMr* mr = static_cast<DpfrMr*>(mhandle);
  for (size_t i = 0; i < devices.size() && i < mr->planeMrs.size(); ++i) {
    if (devices[i].planeId == planeId) return mr->planeMrs[i];
  }
  return nullptr;
}

}  // namespace dpfr
