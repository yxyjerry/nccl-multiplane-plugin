#include "peer_control.h"

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace dpfr {

PeerControl::~PeerControl() {
  close();
}

void PeerControl::reset(int fd) {
  close();
  fd_ = fd;
  rxBuffer_.clear();
}

void PeerControl::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  rxBuffer_.clear();
}

ncclResult_t setSocketNonBlocking(int fd, bool nonBlocking) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return ncclSystemError;
  if (nonBlocking) flags |= O_NONBLOCK;
  else flags &= ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags) == 0 ? ncclSuccess : ncclSystemError;
}

ncclResult_t sendAll(int fd, const void* data, size_t size) {
  const char* ptr = static_cast<const char*>(data);
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::send(fd, ptr + done, size - done, MSG_NOSIGNAL);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    return ncclRemoteError;
  }
  return ncclSuccess;
}

ncclResult_t recvAll(int fd, void* data, size_t size, int timeoutMs) {
  char* ptr = static_cast<char*>(data);
  size_t done = 0;
  while (done < size) {
    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, timeoutMs);
    if (pr == 0) return ncclInternalError;
    if (pr < 0) {
      if (errno == EINTR) continue;
      return ncclSystemError;
    }
    ssize_t n = ::recv(fd, ptr + done, size - done, 0);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) return ncclRemoteError;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return ncclRemoteError;
  }
  return ncclSuccess;
}

ncclResult_t PeerControl::sendMessage(const WireMsg& msg) {
  if (fd_ < 0) return ncclRemoteError;
  std::lock_guard<std::mutex> lock(sendMutex_);
  return sendAll(fd_, &msg, sizeof(msg));
}

ncclResult_t PeerControl::readAvailable(int timeoutMs, bool* timedOut) {
  if (fd_ < 0 || timedOut == nullptr) return ncclInvalidArgument;
  *timedOut = false;
  for (;;) {
    pollfd pfd = {};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, timeoutMs);
    if (pr == 0) {
      *timedOut = true;
      return ncclSuccess;
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      return ncclSystemError;
    }
    if (pfd.revents & POLLNVAL) return ncclRemoteError;
    break;
  }

  char chunk[4096];
  for (;;) {
    ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
    if (n > 0) {
      rxBuffer_.insert(rxBuffer_.end(), chunk, chunk + n);
      continue;
    }
    if (n == 0) return ncclRemoteError;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return ncclSuccess;
    return ncclRemoteError;
  }
}

ncclResult_t PeerControl::popMessage(WireMsg* msg, bool* got) {
  if (msg == nullptr || got == nullptr) return ncclInvalidArgument;
  *got = false;
  if (rxBuffer_.size() < sizeof(WireMsg)) return ncclSuccess;
  std::memcpy(msg, rxBuffer_.data(), sizeof(WireMsg));
  rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + sizeof(WireMsg));
  if (!validMessage(*msg)) return ncclRemoteError;
  *got = true;
  return ncclSuccess;
}

ncclResult_t PeerControl::recvMessage(WireMsg* msg, int timeoutMs) {
  if (fd_ < 0 || msg == nullptr) return ncclInvalidArgument;
  for (;;) {
    bool got = false;
    ncclResult_t ret = popMessage(msg, &got);
    if (ret != ncclSuccess || got) return ret;
    bool timedOut = false;
    ret = readAvailable(timeoutMs, &timedOut);
    if (ret != ncclSuccess) return ret;
    if (timedOut) return ncclInternalError;
  }
}

ncclResult_t PeerControl::progressRecv(WireMsg* msg, bool* got) {
  if (fd_ < 0 || msg == nullptr || got == nullptr) return ncclInvalidArgument;
  ncclResult_t ret = popMessage(msg, got);
  if (ret != ncclSuccess || *got) return ret;
  bool timedOut = false;
  ret = readAvailable(0, &timedOut);
  if (ret != ncclSuccess) return ret;
  if (timedOut) {
    *got = false;
    return ncclSuccess;
  }
  return popMessage(msg, got);
}

}  // namespace dpfr
