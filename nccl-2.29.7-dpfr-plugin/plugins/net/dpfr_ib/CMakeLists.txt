set(SRC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plane_state.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/icmp_probe.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cq_error.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/control_wire.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/peer_control.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/verbs_device.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/verbs_qp.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mr.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qp_slot.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qp_selector.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/request.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/replay_engine.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/monitor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm.cc
)

add_library(nccl-net-dpfr_ib SHARED ${SRC_FILES})

target_include_directories(nccl-net-dpfr_ib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/nccl
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(nccl-net-dpfr_ib PRIVATE cxx_std_17)
target_compile_options(nccl-net-dpfr_ib PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nccl-net-dpfr_ib PRIVATE ibverbs pthread)

set_target_properties(nccl-net-dpfr_ib PROPERTIES
    OUTPUT_NAME "nccl-net-dpfr_ib"
    PREFIX "lib"
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test/unit/plugins
)
