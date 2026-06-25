# DPFR IB NCCL Net Plugin

This is a conservative host-side NCCL net plugin for NCCL 2.29.7. It exposes one
aggregate network device named `DPFR_IB` and internally manages multiple IB
ports as fault planes.

Build:

```bash
make -C plugins/net/dpfr_ib BUILDDIR=/tmp/nccl-dpfr-plugin
```

Core unit tests:

```bash
make -C plugins/net/dpfr_ib/tests
```

Run:

```bash
export NCCL_NET_PLUGIN=dpfr_ib
export NCCL_NET=DPFR_IB
export LD_LIBRARY_PATH=/tmp/nccl-dpfr-plugin:$LD_LIBRARY_PATH
```

Required/recommended configuration:

```bash
export NCCL_DPFR_IB_HCA_LIST=mlx5_0:1:0,mlx5_1:1:1
```

Optional control inputs:

```bash
export NCCL_DPFR_IB_CONTROL_TCP_SERVER=127.0.0.1
export NCCL_DPFR_IB_CONTROL_TCP_PORT=19000
export NCCL_DPFR_IB_PLANE0_PROBE_IP=10.0.0.2
export NCCL_DPFR_IB_PLANE0_PROBE_IFACE=eth0
export NCCL_DPFR_IB_DPFR_UDP_PORT=19767
export NCCL_DPFR_IB_PLANE0_DPFR_IFACE=eth0
```

V1 deliberately uses `maxRecvs=1`, one RC QP per configured plane per NCCL
connection, and strict per-connection ordering. A plane recovery event never
re-admits an old QP directly; it creates a replacement QP, exchanges metadata on
the plugin control socket, commits the replacement, then retires the old QP when
no request still references its generation.

Module layout:

```text
src/plugin.cc          NCCL v11/v10 ABI exports only
src/config.*           environment and text control parsing
src/plane_state.*      plane health table and generation transitions
src/monitor.*          TCP, UDP/DPFR text ingress, and optional ICMP probe
src/control_wire.*     peer-control handle and fixed wire messages
src/peer_control.*     blocking exact-send/recv helpers for peer TCP control
src/verbs_device.*     IB HCA/port discovery and stable per-plane PDs
src/verbs_qp.*         RC QP/CQ lifecycle, RTR/RTS, post send/recv, poll
src/mr.*               per-plane ibv_mr registration against stable PDs
src/qp_slot.*          active/shadow/retired QP ownership
src/qp_selector.*      healthy-plane selection with generation filtering
src/request.*          NCCL request state and CQ wr_id encoding
src/replay_engine.*    sender query/replay state machine
src/comm.*             NCCL listen/connect/accept/isend/irecv/test logic
```

Important V1 boundaries:

- Connection establishment is synchronous after the TCP socket is accepted or
  connected, with timeouts on peer-control reads.
- If every plane is broken or recovering, `isend`/`irecv` returns
  `request=NULL` as backpressure.
- CQ/device/PD/MR failures are treated as non-recoverable comm failures.
- DMABUF and device offload are intentionally unsupported in V1.
