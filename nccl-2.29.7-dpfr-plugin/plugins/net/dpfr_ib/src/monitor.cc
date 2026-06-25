#include "monitor.h"

#include "context.h"
#include "icmp_probe.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace dpfr {
namespace {

bool readLineFromFd(int fd, std::string* line) {
  line->clear();
  char c = 0;
  while (true) {
    ssize_t n = recv(fd, &c, 1, 0);
    if (n == 1) {
      if (c == '\n') return true;
      if (c != '\r') line->push_back(c);
      if (line->size() > 256) return true;
      continue;
    }
    if (n == 0) return !line->empty();
    if (errno == EINTR) continue;
    return false;
  }
}

bool icmpProbe(const PlaneConfig& plane, uint16_t id, uint16_t seq) {
  int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (fd < 0) return false;
  if (!plane.probeIface.empty()) {
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, plane.probeIface.c_str(), plane.probeIface.size() + 1) != 0) {
      DPFR_LOG_WARN("DPFR_IB probe bind failed plane=%d iface=%s", plane.planeId, plane.probeIface.c_str());
      close(fd);
      return false;
    }
  }

  sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  if (inet_pton(AF_INET, plane.probeIp.c_str(), &dst.sin_addr) != 1) {
    close(fd);
    return false;
  }

  char packet[64] = {};
  icmphdr* hdr = reinterpret_cast<icmphdr*>(packet);
  hdr->type = ICMP_ECHO;
  hdr->code = 0;
  hdr->un.echo.id = htons(id);
  hdr->un.echo.sequence = htons(seq);
  hdr->checksum = icmpChecksum(packet, sizeof(packet));
  if (sendto(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
    close(fd);
    return false;
  }

  bool matched = false;
  for (int attempts = 0; attempts < 4 && !matched; ++attempts) {
    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 500) <= 0) break;

    char recvBuf[512];
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    ssize_t n = recvfrom(fd, recvBuf, sizeof(recvBuf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) continue;
    matched = parseIcmpEchoReply(recvBuf, static_cast<size_t>(n), dst.sin_addr.s_addr, id, seq);
  }
  close(fd);
  return matched;
}

}  // namespace

Monitor::Monitor(DpfrContext* context) : context_(context) {}

Monitor::~Monitor() {
  stop();
}

void Monitor::start(const PluginConfig& config) {
  stop_ = false;
  if (config.controlTcpPort > 0) tcpThread_ = std::thread(&Monitor::tcpThreadMain, this, config);
  if (config.dpfrUdpPort > 0) udpThread_ = std::thread(&Monitor::udpThreadMain, this, config);
  bool hasProbe = false;
  for (const PlaneConfig& plane : config.planes) hasProbe = hasProbe || !plane.probeIp.empty();
  if (hasProbe) probeThread_ = std::thread(&Monitor::probeThreadMain, this, config);
}

void Monitor::stop() {
  stop_ = true;
  if (tcpThread_.joinable()) tcpThread_.join();
  if (udpThread_.joinable()) udpThread_.join();
  if (probeThread_.joinable()) probeThread_.join();
}

void Monitor::tcpThreadMain(PluginConfig config) {
  while (!stop_) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (!config.controlTcpServer.empty()) {
      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(config.controlTcpPort));
      if (inet_pton(AF_INET, config.controlTcpServer.c_str(), &addr.sin_addr) != 1 ||
          connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      std::string line;
      while (!stop_ && readLineFromFd(fd, &line)) {
        int plane = -1;
        bool recovered = false;
        if (parsePlaneControlLine(line, &plane, &recovered)) context_->applyPlaneEvent(plane, recovered, "tcp-control", true);
      }
      close(fd);
      continue;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(config.controlTcpPort));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 16) != 0) {
      close(fd);
      return;
    }
    while (!stop_) {
      pollfd pfd = {};
      pfd.fd = fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, 500) <= 0) continue;
      int client = accept(fd, nullptr, nullptr);
      if (client < 0) continue;
      std::string line;
      while (!stop_ && readLineFromFd(client, &line)) {
        int plane = -1;
        bool recovered = false;
        if (parsePlaneControlLine(line, &plane, &recovered)) context_->applyPlaneEvent(plane, recovered, "tcp-control", true);
      }
      close(client);
    }
    close(fd);
  }
}

void Monitor::udpThreadMain(PluginConfig config) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(config.dpfrUdpPort));
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return;
  }
  while (!stop_) {
    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 500) <= 0) continue;
    char buf[256] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) continue;
    int plane = -1;
    bool recovered = false;
    if (parsePlaneControlLine(buf, &plane, &recovered)) context_->applyPlaneEvent(plane, recovered, "dpfr-udp", true);
  }
  close(fd);
}

void Monitor::probeThreadMain(PluginConfig config) {
  int testFd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (testFd < 0) {
    DPFR_LOG_WARN("DPFR_IB ICMP probe disabled: raw socket requires CAP_NET_RAW");
    return;
  }
  close(testFd);
  uint16_t seq = 0;
  uint16_t id = static_cast<uint16_t>(getpid() & 0xffff);
  int misses[kMaxPlanes] = {};
  int hits[kMaxPlanes] = {};
  while (!stop_) {
    for (const PlaneConfig& plane : config.planes) {
      if (plane.probeIp.empty()) continue;
      bool ok = icmpProbe(plane, id, ++seq);
      if (ok) {
        misses[plane.planeId] = 0;
        hits[plane.planeId]++;
        if (hits[plane.planeId] >= 2 && context_->planeTable().health(plane.planeId) == PlaneHealth::Broken) {
          context_->applyPlaneEvent(plane.planeId, true, "probe", true);
        }
      } else {
        hits[plane.planeId] = 0;
        if (++misses[plane.planeId] >= 3) {
          context_->applyPlaneEvent(plane.planeId, false, "probe", true);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace dpfr
