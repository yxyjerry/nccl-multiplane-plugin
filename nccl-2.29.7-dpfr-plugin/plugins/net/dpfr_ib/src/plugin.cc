#include "comm.h"

using dpfr::DpfrNet;

namespace {

ncclResult_t pluginInit(void** ctx, uint64_t commId, ncclNetCommConfig_t* config, ncclDebugLogger_t logger, ncclProfilerCallback_t profiler) {
  return DpfrNet::init(ctx, commId, config, logger, profiler);
}

ncclResult_t pluginInitV10(ncclDebugLogger_t logger, ncclProfilerCallback_t profiler) {
  return DpfrNet::init(nullptr, 0, nullptr, logger, profiler);
}

ncclResult_t pluginGetPropertiesV10(int dev, ncclNetProperties_v10_t* props10) {
  ncclNetProperties_t props11;
  ncclResult_t ret = DpfrNet::getProperties(dev, &props11);
  if (ret != ncclSuccess) return ret;
  props10->name = props11.name;
  props10->pciPath = props11.pciPath;
  props10->guid = props11.guid;
  props10->ptrSupport = props11.ptrSupport;
  props10->regIsGlobal = props11.regIsGlobal;
  props10->forceFlush = props11.forceFlush;
  props10->speed = props11.speed;
  props10->port = props11.port;
  props10->latency = props11.latency;
  props10->maxComms = props11.maxComms;
  props10->maxRecvs = props11.maxRecvs;
  props10->netDeviceType = props11.netDeviceType;
  props10->netDeviceVersion = props11.netDeviceVersion;
  props10->vProps.ndevs = props11.vProps.ndevs;
  for (int i = 0; i < NCCL_NET_MAX_DEVS_PER_NIC; ++i) props10->vProps.devs[i] = props11.vProps.devs[i];
  props10->maxP2pBytes = props11.maxP2pBytes;
  props10->maxCollBytes = props11.maxCollBytes;
  return ncclSuccess;
}

ncclResult_t pluginListenV10(int dev, void* handle, void** listenComm) {
  return DpfrNet::listen(&dpfr::globalContext(), dev, handle, listenComm);
}

ncclResult_t pluginConnectV10(int dev, ncclNetCommConfig_v10_t* config, void* handle, void** sendComm, ncclNetDeviceHandle_v10_t** sendDevComm) {
  return DpfrNet::connect(&dpfr::globalContext(), dev, handle, sendComm, sendDevComm);
}

ncclResult_t pluginMakeVDeviceV10(int* d, ncclNetVDeviceProps_v10_t* props) {
  return DpfrNet::makeVDevice(d, reinterpret_cast<ncclNetVDeviceProps_t*>(props));
}

}  // namespace

extern "C" {

extern const ncclNet_v11_t ncclNetPlugin_v11 = {
    "DPFR_IB",
    pluginInit,
    DpfrNet::devices,
    DpfrNet::getProperties,
    DpfrNet::listen,
    DpfrNet::connect,
    DpfrNet::accept,
    DpfrNet::regMr,
    DpfrNet::regMrDmaBuf,
    DpfrNet::deregMr,
    DpfrNet::isend,
    DpfrNet::irecv,
    DpfrNet::iflush,
    DpfrNet::test,
    DpfrNet::closeSend,
    DpfrNet::closeRecv,
    DpfrNet::closeListen,
    DpfrNet::getDeviceMr,
    DpfrNet::irecvConsumed,
    DpfrNet::makeVDevice,
    DpfrNet::finalize,
    DpfrNet::setNetAttr,
};

extern const ncclNet_v10_t ncclNetPlugin_v10 = {
    "DPFR_IB",
    pluginInitV10,
    DpfrNet::devices,
    pluginGetPropertiesV10,
    pluginListenV10,
    pluginConnectV10,
    DpfrNet::accept,
    DpfrNet::regMr,
    DpfrNet::regMrDmaBuf,
    DpfrNet::deregMr,
    DpfrNet::isend,
    DpfrNet::irecv,
    DpfrNet::iflush,
    DpfrNet::test,
    DpfrNet::closeSend,
    DpfrNet::closeRecv,
    DpfrNet::closeListen,
    DpfrNet::getDeviceMr,
    DpfrNet::irecvConsumed,
    pluginMakeVDeviceV10,
};

}  // extern "C"
