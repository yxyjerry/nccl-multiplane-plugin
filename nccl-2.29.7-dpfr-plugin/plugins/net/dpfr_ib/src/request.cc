#include "request.h"

namespace dpfr {

constexpr uint64_t kWrMagic = 0xd1f0000000000000ULL;
constexpr uint64_t kInternalWrMagic = 0xd1f1000000000000ULL;

uint64_t makeWrId(int requestIndex, uint32_t generation) {
  return kWrMagic | (static_cast<uint64_t>(generation) << 16) | static_cast<uint64_t>(requestIndex & 0xffff);
}

int wrRequestIndex(uint64_t wrId) {
  if ((wrId & 0xffff000000000000ULL) != kWrMagic) return -1;
  return static_cast<int>(wrId & 0xffff);
}

uint32_t wrGeneration(uint64_t wrId) {
  if ((wrId & 0xffff000000000000ULL) != kWrMagic) return 0;
  return static_cast<uint32_t>((wrId >> 16) & 0xffffffff);
}

uint64_t makeInternalWrId(InternalWrKind kind, uint32_t generation) {
  return kInternalWrMagic | (static_cast<uint64_t>(generation) << 16) | static_cast<uint64_t>(kind);
}

bool isInternalWrId(uint64_t wrId) {
  return (wrId & 0xffff000000000000ULL) == kInternalWrMagic;
}

InternalWrKind internalWrKind(uint64_t wrId) {
  return static_cast<InternalWrKind>(wrId & 0xffff);
}

uint32_t internalWrGeneration(uint64_t wrId) {
  if (!isInternalWrId(wrId)) return 0;
  return static_cast<uint32_t>((wrId >> 16) & 0xffffffff);
}

const char* requestStateName(RequestState state) {
  switch (state) {
    case RequestState::Free: return "FREE";
    case RequestState::Posted: return "POSTED";
    case RequestState::NeedsQuery: return "NEEDS_QUERY";
    case RequestState::NeedsReplay: return "NEEDS_REPLAY";
    case RequestState::Done: return "DONE";
    case RequestState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

}  // namespace dpfr
