#ifndef DPFR_IB_REQUEST_H_
#define DPFR_IB_REQUEST_H_

#include "net.h"

#include <cstddef>
#include <cstdint>

namespace dpfr {

enum class RequestKind {
  Send,
  Recv,
  Flush,
};

enum class RequestState {
  Free,
  Posted,
  NeedsQuery,
  NeedsReplay,
  Done,
  Failed,
};

enum class InternalWrKind : uint16_t {
  RecoverySend = 1,
  RecoveryRecv = 2,
};

struct DpfrRequest {
  int index = -1;
  RequestKind kind = RequestKind::Send;
  RequestState state = RequestState::Free;
  uint64_t seq = 0;
  int slotIndex = -1;
  int planeId = -1;
  uint32_t generation = 0;
  void* data = nullptr;
  size_t size = 0;
  int tag = 0;
  void* mhandle = nullptr;
  int completedSize = 0;
  bool replayed = false;
  void* owner = nullptr;
};

uint64_t makeWrId(int requestIndex, uint32_t generation);
int wrRequestIndex(uint64_t wrId);
uint32_t wrGeneration(uint64_t wrId);
uint64_t makeInternalWrId(InternalWrKind kind, uint32_t generation);
bool isInternalWrId(uint64_t wrId);
InternalWrKind internalWrKind(uint64_t wrId);
uint32_t internalWrGeneration(uint64_t wrId);
const char* requestStateName(RequestState state);

}  // namespace dpfr

#endif  // DPFR_IB_REQUEST_H_
