#include "config.h"
#include "cq_error.h"
#include "icmp_probe.h"
#include "peer_control.h"
#include "plane_state.h"
#include "qp_selector.h"
#include "request.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace dpfr;

namespace {

void testConfigParser() {
  std::vector<PlaneConfig> planes;
  std::string error;
  assert(parseHcaList("mlx5_0:1:0,mlx5_1:2:1", &planes, &error) == ncclSuccess);
  assert(planes.size() == 2);
  assert(planes[0].hcaName == "mlx5_0");
  assert(planes[0].port == 1);
  assert(planes[0].planeId == 0);
  assert(planes[1].hcaName == "mlx5_1");
  assert(planes[1].port == 2);
  assert(planes[1].planeId == 1);
  assert(parseHcaList("mlx5_0:nope:0", &planes, &error) == ncclInvalidArgument);

  int plane = -1;
  bool recovered = false;
  assert(parsePlaneControlLine("plane 1 broken", &plane, &recovered));
  assert(plane == 1 && !recovered);
  assert(parsePlaneControlLine("plane=1 recovered", &plane, &recovered));
  assert(plane == 1 && recovered);
}

void testPlaneTable() {
  PlaneTable table;
  table.reset({0, 1});
  assert(table.generation(0) == 1);
  PlaneSnapshot snap;
  assert(table.markBroken(0, "unit", &snap));
  assert(snap.health == PlaneHealth::Broken);
  assert(snap.generation == 2);
  assert(table.markBroken(0, "unit", &snap));
  assert(snap.generation == 2);
  assert(table.markRecovering(0, "unit", &snap));
  assert(snap.health == PlaneHealth::Recovering);
  assert(snap.generation == 3);
  assert(table.markHealthy(0, "unit", &snap));
  assert(snap.health == PlaneHealth::Healthy);
  assert(snap.generation == 3);

  bool applied = true;
  assert(table.applyRemoteEvent(0, PlaneHealth::Broken, 2, "stale", &snap, &applied));
  assert(!applied);
  assert(snap.health == PlaneHealth::Healthy);
  assert(snap.generation == 3);
  assert(table.applyRemoteEvent(0, PlaneHealth::Broken, 4, "peer", &snap, &applied));
  assert(applied);
  assert(snap.health == PlaneHealth::Broken);
  assert(snap.generation == 4);
  assert(table.applyRemoteEvent(0, PlaneHealth::Recovering, 4, "same-gen-conflict", &snap, &applied));
  assert(!applied);
  assert(snap.health == PlaneHealth::Broken);
  assert(snap.generation == 4);
  assert(table.applyRemoteEvent(0, PlaneHealth::Recovering, 5, "peer", &snap, &applied));
  assert(applied);
  assert(snap.health == PlaneHealth::Recovering);
  assert(snap.generation == 5);
  assert(!table.markHealthyIfGeneration(0, 4, "old-commit", &snap));
  assert(snap.health == PlaneHealth::Recovering);
  assert(snap.generation == 5);
  assert(table.markHealthyIfGeneration(0, 5, "commit", &snap));
  assert(snap.health == PlaneHealth::Healthy);
  assert(snap.generation == 5);
  assert(table.markHealthyIfGeneration(0, 5, "idempotent-commit", &snap));
  assert(snap.health == PlaneHealth::Healthy);
  assert(snap.generation == 5);
}

void testSelectorAndWrId() {
  PlaneTable table;
  table.reset({0, 1});
  std::vector<QpSlot> slots(2);
  for (int i = 0; i < 2; ++i) {
    slots[i].planeId = i;
    slots[i].state = QpSlotState::Active;
    slots[i].active.ready = true;
    slots[i].active.generation = 1;
    slots[i].active.qp = reinterpret_cast<ibv_qp*>(static_cast<uintptr_t>(i + 1));
  }

  QpSelector selector;
  assert(selector.select(&slots, table, 0) == 0);
  PlaneSnapshot snap;
  table.markBroken(0, "unit", &snap);
  assert(selector.select(&slots, table, 0) == 1);
  table.markRecovering(0, "unit", &snap);
  table.markHealthy(0, "unit", &snap);
  assert(selector.select(&slots, table, 0) == 1);
  slots[0].active.generation = snap.generation;
  assert(selector.select(&slots, table, 0) == 0);

  uint64_t wrid = makeWrId(7, 42);
  assert(wrRequestIndex(wrid) == 7);
  assert(wrGeneration(wrid) == 42);

  uint64_t internal = makeInternalWrId(InternalWrKind::RecoveryRecv, 43);
  assert(isInternalWrId(internal));
  assert(internalWrKind(internal) == InternalWrKind::RecoveryRecv);
  assert(internalWrGeneration(internal) == 43);

  slots[0].resetProbe(99);
  slots[0].probeSendDone = true;
  slots[0].probeRecvDone = true;
  slots[0].probeFailed = true;
  slots[0].clearProbe();
  assert(slots[0].probeGeneration == 0);
  assert(!slots[0].probeSendDone);
  assert(!slots[0].probeRecvDone);
  assert(!slots[0].probeFailed);
}

void testIcmpParser() {
  char packet[sizeof(iphdr) + sizeof(icmphdr)] = {};
  iphdr* ip = reinterpret_cast<iphdr*>(packet);
  icmphdr* icmp = reinterpret_cast<icmphdr*>(packet + sizeof(iphdr));
  ip->version = 4;
  ip->ihl = sizeof(iphdr) / 4;
  ip->protocol = IPPROTO_ICMP;
  inet_pton(AF_INET, "10.10.1.2", &ip->saddr);
  icmp->type = ICMP_ECHOREPLY;
  icmp->code = 0;
  icmp->un.echo.id = htons(123);
  icmp->un.echo.sequence = htons(456);

  uint32_t expected = 0;
  inet_pton(AF_INET, "10.10.1.2", &expected);
  assert(parseIcmpEchoReply(packet, sizeof(packet), expected, 123, 456));
  assert(!parseIcmpEchoReply(packet, sizeof(packet), expected, 123, 457));
  icmp->type = ICMP_ECHO;
  assert(!parseIcmpEchoReply(packet, sizeof(packet), expected, 123, 456));
}

void testCqErrorClassifier() {
  assert(classifyCqStatus(IBV_WC_SUCCESS, true) == CqErrorAction::Success);
  assert(classifyCqStatus(IBV_WC_RETRY_EXC_ERR, true) == CqErrorAction::TransportRecoverable);
  assert(classifyCqStatus(IBV_WC_RNR_RETRY_EXC_ERR, true) == CqErrorAction::TransportRecoverable);
  assert(classifyCqStatus(IBV_WC_WR_FLUSH_ERR, true) == CqErrorAction::TransportRecoverable);
  assert(classifyCqStatus(IBV_WC_WR_FLUSH_ERR, false) == CqErrorAction::StaleNonFatal);
  assert(classifyCqStatus(IBV_WC_LOC_PROT_ERR, true) == CqErrorAction::RequestFatal);
}

void testPeerControlPartialRecv() {
  int fds[2] = {-1, -1};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  PeerControl peer;
  peer.reset(fds[0]);

  WireMsg msg = makeMessage(WireMsgType::PlaneEvent);
  msg.planeId = 1;
  msg.generation = 7;
  msg.done = 0;

  const char* raw = reinterpret_cast<const char*>(&msg);
  constexpr size_t firstChunk = 17;
  assert(sendAll(fds[1], raw, firstChunk) == ncclSuccess);

  WireMsg out;
  bool got = true;
  assert(peer.progressRecv(&out, &got) == ncclSuccess);
  assert(!got);

  assert(sendAll(fds[1], raw + firstChunk, sizeof(msg) - firstChunk) == ncclSuccess);
  assert(peer.progressRecv(&out, &got) == ncclSuccess);
  assert(got);
  assert(out.type == WireMsgType::PlaneEvent);
  assert(out.planeId == 1);
  assert(out.generation == 7);

  ::close(fds[1]);
}

}  // namespace

int main() {
  testConfigParser();
  testPlaneTable();
  testSelectorAndWrId();
  testIcmpParser();
  testCqErrorClassifier();
  testPeerControlPartialRecv();
  std::cout << "dpfr_ib core tests passed\n";
  return 0;
}
