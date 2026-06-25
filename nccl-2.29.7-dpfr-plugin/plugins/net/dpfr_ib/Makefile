#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#

.DEFAULT_GOAL: build
include ../../../makefiles/common.mk

BUILDDIR ?= .
CXX ?= g++

SRC_FILES := \
	src/plugin.cc \
	src/log.cc \
	src/config.cc \
	src/plane_state.cc \
	src/icmp_probe.cc \
	src/cq_error.cc \
	src/control_wire.cc \
	src/peer_control.cc \
	src/verbs_device.cc \
	src/verbs_qp.cc \
	src/mr.cc \
	src/qp_slot.cc \
	src/qp_selector.cc \
	src/request.cc \
	src/replay_engine.cc \
	src/monitor.cc \
	src/context.cc \
	src/comm.cc
TARGET := ${BUILDDIR}/libnccl-net-dpfr_ib.so

build: ${TARGET}

${TARGET}: ${SRC_FILES}
	@printf "Compiling  %-35s > %s\n" $< $@
	@mkdir -p ${BUILDDIR}
	$(CXX) -std=c++17 -Inccl -Isrc -fPIC -shared -Wall -Wextra -Wno-unused-parameter -o $@ $^ -libverbs -lpthread

clean:
	rm -f ${TARGET}
