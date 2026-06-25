#include "qp_slot.h"

namespace dpfr {

bool QpSlot::selectable(const PlaneSnapshot& plane) const {
  return state == QpSlotState::Active &&
         active.qp != nullptr &&
         active.ready &&
         plane.health == PlaneHealth::Healthy &&
         active.generation == plane.generation;
}

uint32_t QpSlot::activeGeneration() const {
  return active.generation;
}

void QpSlot::quarantine() {
  if (state == QpSlotState::Active) state = QpSlotState::Quarantined;
}

void QpSlot::resetProbe(uint32_t generation) {
  probeGeneration = generation;
  probeSendDone = false;
  probeRecvDone = false;
  probeFailed = false;
}

void QpSlot::clearProbe() {
  resetProbe(0);
}

void QpSlot::abortRecovery() {
  destroyEndpoint(&shadow);
  clearProbe();
  if (state == QpSlotState::Rebuilding || state == QpSlotState::ReadyPending) {
    state = active.qp != nullptr ? QpSlotState::Quarantined : QpSlotState::Dead;
  }
}

void QpSlot::markDead() {
  state = QpSlotState::Dead;
  destroyEndpoint(&active);
  destroyEndpoint(&shadow);
  for (QpEndpoint& endpoint : retired) destroyEndpoint(&endpoint);
  retired.clear();
}

void QpSlot::retireActive() {
  if (active.qp != nullptr) {
    retired.push_back(active);
    active = QpEndpoint();
  }
}

const char* qpSlotStateName(QpSlotState state) {
  switch (state) {
    case QpSlotState::Active: return "ACTIVE";
    case QpSlotState::Quarantined: return "QUARANTINED";
    case QpSlotState::Rebuilding: return "REBUILDING";
    case QpSlotState::ReadyPending: return "READY_PENDING";
    case QpSlotState::Draining: return "DRAINING";
    case QpSlotState::Dead: return "DEAD";
  }
  return "UNKNOWN";
}

}  // namespace dpfr
