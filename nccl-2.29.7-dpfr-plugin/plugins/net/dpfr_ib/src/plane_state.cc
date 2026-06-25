#include "plane_state.h"

#include <algorithm>

namespace dpfr {

uint64_t PlaneTable::nowUsec() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

ncclResult_t PlaneTable::reset(const std::vector<int>& planeIds) {
  std::lock_guard<std::mutex> lock(mutex_);
  planes_.clear();
  for (int planeId : planeIds) {
    PlaneSnapshot plane;
    plane.planeId = planeId;
    plane.health = PlaneHealth::Healthy;
    plane.generation = 1;
    plane.updatedUsec = nowUsec();
    plane.source = "init";
    planes_.push_back(plane);
  }
  std::sort(planes_.begin(), planes_.end(), [](const PlaneSnapshot& a, const PlaneSnapshot& b) {
    return a.planeId < b.planeId;
  });
  return ncclSuccess;
}

PlaneSnapshot* PlaneTable::findLocked(int planeId) {
  for (PlaneSnapshot& plane : planes_) {
    if (plane.planeId == planeId) return &plane;
  }
  return nullptr;
}

const PlaneSnapshot* PlaneTable::findLocked(int planeId) const {
  for (const PlaneSnapshot& plane : planes_) {
    if (plane.planeId == planeId) return &plane;
  }
  return nullptr;
}

bool PlaneTable::markBroken(int planeId, const char* source, PlaneSnapshot* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  PlaneSnapshot* plane = findLocked(planeId);
  if (plane == nullptr) return false;
  if (plane->health != PlaneHealth::Broken) {
    plane->generation++;
    plane->health = PlaneHealth::Broken;
    plane->updatedUsec = nowUsec();
    plane->source = source == nullptr ? "unknown" : source;
  }
  if (out) *out = *plane;
  return true;
}

bool PlaneTable::markRecovering(int planeId, const char* source, PlaneSnapshot* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  PlaneSnapshot* plane = findLocked(planeId);
  if (plane == nullptr) return false;
  if (plane->health != PlaneHealth::Recovering) {
    plane->generation++;
    plane->health = PlaneHealth::Recovering;
    plane->updatedUsec = nowUsec();
    plane->source = source == nullptr ? "unknown" : source;
  }
  if (out) *out = *plane;
  return true;
}

bool PlaneTable::markHealthy(int planeId, const char* source, PlaneSnapshot* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  PlaneSnapshot* plane = findLocked(planeId);
  if (plane == nullptr) return false;
  plane->health = PlaneHealth::Healthy;
  plane->updatedUsec = nowUsec();
  plane->source = source == nullptr ? "unknown" : source;
  if (out) *out = *plane;
  return true;
}

bool PlaneTable::applyRemoteEvent(int planeId, PlaneHealth health, uint32_t generation, const char* source,
                                  PlaneSnapshot* out, bool* applied) {
  std::lock_guard<std::mutex> lock(mutex_);
  PlaneSnapshot* plane = findLocked(planeId);
  if (applied) *applied = false;
  if (plane == nullptr || generation == 0) return false;
  if (generation < plane->generation) {
    if (out) *out = *plane;
    return true;
  }
  if (generation == plane->generation && plane->health != health) {
    if (out) *out = *plane;
    return true;
  }
  if (generation > plane->generation || plane->health != health) {
    plane->generation = generation;
    plane->health = health;
    plane->updatedUsec = nowUsec();
    plane->source = source == nullptr ? "unknown" : source;
    if (applied) *applied = true;
  }
  if (out) *out = *plane;
  return true;
}

bool PlaneTable::markHealthyIfGeneration(int planeId, uint32_t generation, const char* source, PlaneSnapshot* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  PlaneSnapshot* plane = findLocked(planeId);
  if (plane == nullptr || plane->generation != generation ||
      (plane->health != PlaneHealth::Recovering && plane->health != PlaneHealth::Healthy)) {
    if (out && plane != nullptr) *out = *plane;
    return false;
  }
  if (plane->health == PlaneHealth::Healthy) {
    if (out) *out = *plane;
    return true;
  }
  plane->health = PlaneHealth::Healthy;
  plane->updatedUsec = nowUsec();
  plane->source = source == nullptr ? "unknown" : source;
  if (out) *out = *plane;
  return true;
}

bool PlaneTable::get(int planeId, PlaneSnapshot* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const PlaneSnapshot* plane = findLocked(planeId);
  if (plane == nullptr) return false;
  if (out) *out = *plane;
  return true;
}

PlaneHealth PlaneTable::health(int planeId) const {
  PlaneSnapshot plane;
  return get(planeId, &plane) ? plane.health : PlaneHealth::Broken;
}

uint32_t PlaneTable::generation(int planeId) const {
  PlaneSnapshot plane;
  return get(planeId, &plane) ? plane.generation : 0;
}

std::vector<PlaneSnapshot> PlaneTable::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return planes_;
}

const char* planeHealthName(PlaneHealth health) {
  switch (health) {
    case PlaneHealth::Healthy: return "HEALTHY";
    case PlaneHealth::Broken: return "BROKEN";
    case PlaneHealth::Recovering: return "RECOVERING";
  }
  return "UNKNOWN";
}

}  // namespace dpfr
