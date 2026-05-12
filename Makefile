CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
LDFLAGS ?=
LDLIBS_WT_AGENT = -lhiredis -ljansson -lsqlite3

.PHONY: all clean test

all: build/wt-agent

build/wt-agent: src/worker/wt_agent.c
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS_WT_AGENT)

test: all
	./tests/integration/wt-agent-single-task.sh
	./tests/integration/wtctl-dispatch-order.sh

clean:
	rm -rf build
