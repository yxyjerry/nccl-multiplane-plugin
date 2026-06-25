#include "qp_selector.h"

namespace dpfr {

int QpSelector::select(std::vector<QpSlot>* slots, const PlaneTable& planes, uint64_t seq) {
  if (slots == nullptr || slots->empty()) return -1;
  size_t start = static_cast<size_t>(seq % slots->size());
  for (size_t i = 0; i < slots->size(); ++i) {
    size_t index = (start + i) % slots->size();
    PlaneSnapshot plane;
    if (!planes.get((*slots)[index].planeId, &plane)) continue;
    if ((*slots)[index].selectable(plane)) return static_cast<int>(index);
  }
  return -1;
}

}  // namespace dpfr
