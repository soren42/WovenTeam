CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS ?=
LDLIBS ?=
SQLITE_LIBS ?= -lsqlite3

ifeq ($(DEBUG),1)
  CFLAGS := $(filter-out -O2,$(CFLAGS))
  CFLAGS += -g -O0 -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
endif

BUILD_DIR := build
COMMON_OBJS := \
	$(BUILD_DIR)/wt_config.o \
	$(BUILD_DIR)/wt_json.o \
	$(BUILD_DIR)/wt_message.o \
	$(BUILD_DIR)/wt_room_store.o \
	$(BUILD_DIR)/wt_task_store.o \
	$(BUILD_DIR)/wt_time.o

.PHONY: all clean run-roomd run-demo harness-check test-smoke test-harness-check test-task-assignment test-codex-adapter test-cli-artifact-adapter test-artifact-viewer test-adapter-capabilities test-adapter-preflight test-initiatives test-agent-workload-control test-routing-gates test-manager-subtasks test-token-config test-task-projection test-quotas-ops test-phase1-e2e test install-roomd-service install-agent-services

all: $(BUILD_DIR)/wt-roomd $(BUILD_DIR)/wt-say $(BUILD_DIR)/wt-tail $(BUILD_DIR)/wt-agent

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/wt-roomd: $(COMMON_OBJS) $(BUILD_DIR)/wt_task_projection.o $(BUILD_DIR)/wt_http.o $(BUILD_DIR)/wt_roomd.o
	$(CC) $(LDFLAGS) -o $@ $^ $(SQLITE_LIBS) $(LDLIBS)

$(BUILD_DIR)/wt-say: $(COMMON_OBJS) $(BUILD_DIR)/wt_say.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/wt-tail: $(COMMON_OBJS) $(BUILD_DIR)/wt_tail.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/wt-agent: $(COMMON_OBJS) $(BUILD_DIR)/wt_agent.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

run-roomd: $(BUILD_DIR)/wt-roomd
	./$(BUILD_DIR)/wt-roomd --config config/woventeam-phase0.conf

run-demo: all
	mkdir -p data
	./$(BUILD_DIR)/wt-say ceo all "Claude, ChatGPT, Gemini: each confirm that you can read and respond through the shared room."
	./$(BUILD_DIR)/wt-agent --agent claude --once
	./$(BUILD_DIR)/wt-agent --agent chatgpt --once
	./$(BUILD_DIR)/wt-agent --agent gemini --once
	./$(BUILD_DIR)/wt-tail --limit 20

harness-check:
	./bin/wt-harness-check

test-smoke: all
	./tests/smoke-phase0-room.sh

test-harness-check:
	./tests/integration/wt-harness-check.sh

test-task-assignment: all
	./tests/integration/wt-task-assignment-path.sh

test-codex-adapter: all
	./tests/integration/wt-codex-adapter.sh

test-cli-artifact-adapter: all
	./tests/integration/wt-cli-artifact-adapter.sh

test-artifact-viewer: all
	./tests/integration/wt-artifact-viewer.sh

test-adapter-capabilities: all
	./tests/integration/wt-adapter-capabilities.sh

test-adapter-preflight: all
	./tests/integration/wt-adapter-preflight.sh

test-initiatives: all
	./tests/integration/wt-initiatives.sh

test-agent-workload-control: all
	./tests/integration/wt-agent-workload-control.sh

test-routing-gates: all
	./tests/integration/wt-routing-gates.sh

test-manager-subtasks: all
	./tests/integration/wt-manager-subtasks.sh

test-token-config: all
	./tests/integration/wt-token-config.sh

test-task-projection: all
	./tests/integration/wt-task-projection.sh

test-quotas-ops: all
	./tests/integration/wt-quotas-ops.sh

test-phase1-e2e: all
	./tests/integration/wt-phase1-e2e.sh

test: test-smoke test-harness-check test-task-assignment test-codex-adapter test-cli-artifact-adapter test-artifact-viewer test-adapter-capabilities test-adapter-preflight test-initiatives test-agent-workload-control test-routing-gates test-manager-subtasks test-token-config test-task-projection test-quotas-ops test-phase1-e2e

install-roomd-service: all
	sudo install -m 0644 deploy/systemd/wt-roomd.service /etc/systemd/system/
	sudo systemctl daemon-reload
	sudo systemctl enable --now wt-roomd.service

install-agent-services: all
	sudo install -m 0644 deploy/systemd/wt-agent@.service /etc/systemd/system/
	sudo systemctl daemon-reload
	sudo systemctl disable --now wt-agent@qwen.service || true
	sudo systemctl reset-failed 'wt-agent@*.service' || true
	sudo systemctl enable --now wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service
	sudo systemctl restart wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service

clean:
	rm -rf $(BUILD_DIR)
