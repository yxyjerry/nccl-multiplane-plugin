#ifndef DPFR_IB_CONTROL_WIRE_H_
#define DPFR_IB_CONTROL_WIRE_H_

#include "config.h"

#include <cstdint>
#include <cstring>

namespace dpfr {

constexpr uint32_t kWireMagic = 0x44504652;  // DPFR
constexpr uint32_t kWireVersion = 1;
constexpr uint32_t kHandleMagic = 0x44484652;  // DHFR
constexpr int kControlTextBytes = 64;

enum class WireMsgType : uint32_t {
  InitReq = 1,
  InitResp = 2,
  PlaneEvent = 3,
  Query = 4,
  QueryResp = 5,
  RecoveryReq = 6,
  RecoveryResp = 7,
  RecoveryReady = 8,
  RecoveryAck = 9,
  Error = 10,
};

enum class WireStatus : uint32_t {
  Ok = 0,
  Retry = 1,
  Failed = 2,
};

struct DpfrNetHandle {
  uint32_t magic = kHandleMagic;
  uint32_t ipv4 = 0;
  uint16_t port = 0;
  uint16_t reserved = 0;
  uint64_t nonce = 0;
  char pad[104] = {};
};
static_assert(sizeof(DpfrNetHandle) <= NCCL_NET_HANDLE_MAXSIZE, "NCCL handle too large");

struct WireQpInfo {
  uint32_t planeId = 0;
  uint32_t generation = 0;
  uint32_t qpn = 0;
  uint32_t lid = 0;
  uint32_t psn = 0;
  uint32_t mtu = 0;
  uint32_t gidIndex = 0;
  uint32_t linkLayer = 0;
  uint8_t gid[16] = {};
};

struct WireMsg {
  uint32_t magic = kWireMagic;
  uint32_t version = kWireVersion;
  WireMsgType type = WireMsgType::Error;
  WireStatus status = WireStatus::Ok;
  uint64_t seq = 0;
  uint32_t planeId = 0;
  uint32_t generation = 0;
  uint32_t done = 0;
  uint32_t nQps = 0;
  WireQpInfo qps[kMaxPlanes] = {};
  char text[kControlTextBytes] = {};
};

WireMsg makeMessage(WireMsgType type);
bool validHandle(const DpfrNetHandle& handle);
bool validMessage(const WireMsg& msg);
bool validQpInfo(const WireQpInfo& info);

}  // namespace dpfr

#endif  // DPFR_IB_CONTROL_WIRE_H_
