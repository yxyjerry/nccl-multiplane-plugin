#include "control_wire.h"

namespace dpfr {

WireMsg makeMessage(WireMsgType type) {
  WireMsg msg;
  msg.magic = kWireMagic;
  msg.version = kWireVersion;
  msg.type = type;
  msg.status = WireStatus::Ok;
  return msg;
}

bool validHandle(const DpfrNetHandle& handle) {
  return handle.magic == kHandleMagic && handle.ipv4 != 0 && handle.port != 0;
}

bool validMessage(const WireMsg& msg) {
  return msg.magic == kWireMagic && msg.version == kWireVersion && msg.nQps <= kMaxPlanes;
}

bool validQpInfo(const WireQpInfo& info) {
  if (info.planeId >= static_cast<uint32_t>(kMaxPlanes)) return false;
  if (info.generation == 0 || info.qpn == 0 || info.mtu == 0) return false;
  if (info.psn > 0x00ffffffu) return false;
  bool gidZero = true;
  for (uint8_t byte : info.gid) gidZero = gidZero && byte == 0;
  if (info.lid == 0 && gidZero) return false;
  return true;
}

}  // namespace dpfr
