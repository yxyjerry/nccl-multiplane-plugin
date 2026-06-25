#ifndef DPFR_IB_ICMP_PROBE_H_
#define DPFR_IB_ICMP_PROBE_H_

#include <cstddef>
#include <cstdint>

namespace dpfr {

uint16_t icmpChecksum(const void* data, size_t len);
bool parseIcmpEchoReply(const void* packet, size_t len, uint32_t expectedSrcNbo, uint16_t expectedId, uint16_t expectedSeq);

}  // namespace dpfr

#endif  // DPFR_IB_ICMP_PROBE_H_
