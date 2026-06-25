# 1. 编译插件 (Compile)

指定一个构建目录（例如 /tmp/nccl-dpfr-plugin），编译出来的 .so 会放在那里

make -C plugins/net/dpfr_ib BUILDDIR=/tmp/nccl-dpfr-plugin

编译成功后，您会在 /tmp/nccl-dpfr-plugin 目录下看到生成的 libnccl-net-dpfr_ib.so 或 libnccl-net.so 文件。

# 2. 运行配置 (Run)
这个插件与普通的 NCCL 插件不同，它内部管理了多个 IB 端口作为“容错平面 (Plane)”，所以必须要配置 NCCL_DPFR_IB_HCA_LIST 环境变量告诉插件要使用哪些网卡和端口。

在使用 mpirun 或 torchrun 运行你的测试或模型时，务必带上以下环境变量：

1. 告诉系统去哪里加载插件（刚刚编译输出的目录）

export LD_LIBRARY_PATH=/tmp/nccl-dpfr-plugin:$LD_LIBRARY_PATH

2. 强制 NCCL 使用这个 DPFR_IB 插件

export NCCL_NET_PLUGIN=dpfr_ib

export NCCL_NET=DPFR_IB

3. 【必填】配置你的容错平面映射（Plane 列表）

格式：网卡名:端口号:PlaneID

下面表示：mlx5_0 端口 1 映射为 Plane 0，mlx5_1 端口 1 映射为 Plane 1

export NCCL_DPFR_IB_HCA_LIST=mlx5_0:1:0,mlx5_1:1:1

 4. （可选）开启日志，方便观察插件是否挂载成功

export NCCL_DEBUG=INFO


# 3. 使用 mpirun 测试的完整示例命令
如果您用 nccl-tests 来测试（假设已经在 /path/to/nccl-tests 编译好），可以直接一条命令拉起：

mpirun -np 8 
  -H node1:4,node2:4 \
  --bind-to none \
  -x LD_LIBRARY_PATH=/tmp/nccl-dpfr-plugin:$LD_LIBRARY_PATH \
  -x NCCL_NET_PLUGIN=dpfr_ib \
  -x NCCL_NET=DPFR_IB \
  -x NCCL_DPFR_IB_HCA_LIST="mlx5_0:1:0,mlx5_1:1:1" \
  -x NCCL_DEBUG=INFO \
  /path/to/nccl-tests/build/all_reduce_perf -b 8 -e 128M -f 2 -g 1
运行成功的标志： 看终端输出。只要看到类似 [node1] NET/Plugin : Plugin load (DPFR_IB) success，或者 DPFR_IB initialized commId=... planes=... 的日志，就说明插件已经正常编译并挂载成功，代码正在工作！

## ICMP 探测 (可选功能)

为了实现更快速的故障发现（例如：网线被拔掉、网络闪断等），您可以为每个 Plane（容错平面）配置目标 IP 和物理网络接口，以启用后台的主动 ICMP 探测（Ping）。

* `NCCL_DPFR_IB_PLANE<id>_PROBE_IP`: 要 Ping 的目标 IP 地址（通常配置为交换机网关 IP 或对端节点的 IP）。
* `NCCL_DPFR_IB_PLANE<id>_PROBE_IFACE`: 用于发送 Ping 探测包的物理网卡接口（例如 `eth0`）。这一项**非常重要**：插件在底层利用了 `SO_BINDTODEVICE` 机制来绕过系统的常规路由表，强制这些 ICMP 探测包必须从该 Plane 绑定的确切物理网口发出去。

**配置示例：**
```bash
# 将 mlx5_0 的端口 1 映射为 Plane 0
export NCCL_DPFR_IB_HCA_LIST=mlx5_0:1:0

# 开启针对 Plane 0 的 ICMP 探测：强制从 eth0 发送，目标为 10.0.0.2
export NCCL_DPFR_IB_PLANE0_PROBE_IP=10.0.0.2
export NCCL_DPFR_IB_PLANE0_PROBE_IFACE=eth0
