#ifndef DPFR_IB_PEER_CONTROL_H_
#define DPFR_IB_PEER_CONTROL_H_

#include "control_wire.h"

#include "net.h"

#include <mutex>
#include <vector>

namespace dpfr {

class PeerControl {
 public:
  PeerControl() = default;
  ~PeerControl();

  PeerControl(const PeerControl&) = delete;
  PeerControl& operator=(const PeerControl&) = delete;

  void reset(int fd);
  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void close();

  ncclResult_t sendMessage(const WireMsg& msg);
  ncclResult_t recvMessage(WireMsg* msg, int timeoutMs);
  ncclResult_t progressRecv(WireMsg* msg, bool* got);

 private:
  ncclResult_t readAvailable(int timeoutMs, bool* timedOut);
  ncclResult_t popMessage(WireMsg* msg, bool* got);

  int fd_ = -1;
  std::mutex sendMutex_;
  std::vector<char> rxBuffer_;
};

ncclResult_t setSocketNonBlocking(int fd, bool nonBlocking);
ncclResult_t sendAll(int fd, const void* data, size_t size);
ncclResult_t recvAll(int fd, void* data, size_t size, int timeoutMs);

}  // namespace dpfr

#endif  // DPFR_IB_PEER_CONTROL_H_
