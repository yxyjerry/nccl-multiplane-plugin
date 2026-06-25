#ifndef DPFR_IB_QP_SLOT_H_
#define DPFR_IB_QP_SLOT_H_

#include "plane_state.h"
#include "verbs_qp.h"

#include <vector>

namespace dpfr {

enum class QpSlotState {
  Active,
  Quarantined,
  Rebuilding,
  ReadyPending,
  Draining,
  Dead,
};

struct QpSlot {
  int planeId = 0;
  int planeIndex = 0;
  QpSlotState state = QpSlotState::Dead;
  QpEndpoint active;
  QpEndpoint shadow;
  std::vector<QpEndpoint> retired;
  uint32_t probeGeneration = 0;
  bool probeSendDone = false;
  bool probeRecvDone = false;
  bool probeFailed = false;

  bool selectable(const PlaneSnapshot& plane) const;
  uint32_t activeGeneration() const;
  void quarantine();
  void resetProbe(uint32_t generation);
  void clearProbe();
  void abortRecovery();
  void markDead();
  void retireActive();
};

const char* qpSlotStateName(QpSlotState state);

}  // namespace dpfr

#endif  // DPFR_IB_QP_SLOT_H_
