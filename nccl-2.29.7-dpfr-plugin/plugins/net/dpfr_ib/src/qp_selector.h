#ifndef DPFR_IB_QP_SELECTOR_H_
#define DPFR_IB_QP_SELECTOR_H_

#include "plane_state.h"
#include "qp_slot.h"

#include <vector>

namespace dpfr {

class QpSelector {
 public:
  int select(std::vector<QpSlot>* slots, const PlaneTable& planes, uint64_t seq);
};

}  // namespace dpfr

#endif  // DPFR_IB_QP_SELECTOR_H_
