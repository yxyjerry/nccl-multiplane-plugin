#include "icmp_probe.h"

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

namespace dpfr {

uint16_t icmpChecksum(const void* data, size_t len) {
  const uint16_t* words = static_cast<const uint16_t*>(data);
  uint32_t sum = 0;
  while (len > 1) {
    sum += *words++;
    len -= 2;
  }
  if (len == 1) sum += *reinterpret_cast<const uint8_t*>(words);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

bool parseIcmpEchoReply(const void* packet, size_t len, uint32_t expectedSrcNbo, uint16_t expectedId, uint16_t expectedSeq) {
  if (packet == nullptr || len < sizeof(iphdr)) return false;
  const uint8_t* bytes = static_cast<const uint8_t*>(packet);
  const iphdr* ip = reinterpret_cast<const iphdr*>(bytes);
  size_t ipHeaderLen = static_cast<size_t>(ip->ihl) * 4;
  if (ip->version != 4 || ipHeaderLen < sizeof(iphdr) || len < ipHeaderLen + sizeof(icmphdr)) return false;
  if (ip->protocol != IPPROTO_ICMP || ip->saddr != expectedSrcNbo) return false;

  const icmphdr* icmp = reinterpret_cast<const icmphdr*>(bytes + ipHeaderLen);
  if (icmp->type != ICMP_ECHOREPLY || icmp->code != 0) return false;
  if (ntohs(icmp->un.echo.id) != expectedId) return false;
  if (ntohs(icmp->un.echo.sequence) != expectedSeq) return false;
  return true;
}

}  // namespace dpfr
