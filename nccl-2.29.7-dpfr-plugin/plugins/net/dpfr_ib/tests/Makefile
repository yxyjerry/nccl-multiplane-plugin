.DEFAULT_GOAL: test

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wno-unused-parameter -I../nccl -I../src
LDFLAGS ?= -libverbs -lpthread

TEST_BIN := /tmp/dpfr_ib_test_core

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): test_core.cc \
	../src/log.cc \
	../src/config.cc \
	../src/plane_state.cc \
	../src/icmp_probe.cc \
	../src/cq_error.cc \
	../src/control_wire.cc \
	../src/peer_control.cc \
	../src/request.cc \
	../src/qp_slot.cc \
	../src/qp_selector.cc \
	../src/verbs_qp.cc
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TEST_BIN)
