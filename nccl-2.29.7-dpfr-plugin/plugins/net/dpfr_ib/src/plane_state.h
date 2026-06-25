#ifndef DPFR_IB_PLANE_STATE_H_
#define DPFR_IB_PLANE_STATE_H_

#include "net.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dpfr {

enum class PlaneHealth : uint32_t {
  Healthy = 0,
  Broken = 1,
  Recovering = 2,
};

struct PlaneSnapshot {
  int planeId = 0;
  PlaneHealth health = PlaneHealth::Healthy;
  uint32_t generation = 1;
  uint64_t updatedUsec = 0;
  std::string source;
};

class PlaneTable {
 public:
  ncclResult_t reset(const std::vector<int>& planeIds);
  bool markBroken(int planeId, const char* source, PlaneSnapshot* out);
  bool markRecovering(int planeId, const char* source, PlaneSnapshot* out);
  bool markHealthy(int planeId, const char* source, PlaneSnapshot* out);
  bool applyRemoteEvent(int planeId, PlaneHealth health, uint32_t generation, const char* source, PlaneSnapshot* out, bool* applied);
  bool markHealthyIfGeneration(int planeId, uint32_t generation, const char* source, PlaneSnapshot* out);
  bool get(int planeId, PlaneSnapshot* out) const;
  PlaneHealth health(int planeId) const;
  uint32_t generation(int planeId) const;
  std::vector<PlaneSnapshot> snapshot() const;

 private:
  PlaneSnapshot* findLocked(int planeId);
  const PlaneSnapshot* findLocked(int planeId) const;
  static uint64_t nowUsec();

  mutable std::mutex mutex_;
  std::vector<PlaneSnapshot> planes_;
};

const char* planeHealthName(PlaneHealth health);

}  // namespace dpfr

#endif  // DPFR_IB_PLANE_STATE_H_
