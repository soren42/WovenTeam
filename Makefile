CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS ?=

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
	$(BUILD_DIR)/wt_time.o

.PHONY: all clean run-roomd run-demo test-smoke test install-roomd-service

all: $(BUILD_DIR)/wt-roomd $(BUILD_DIR)/wt-say $(BUILD_DIR)/wt-tail $(BUILD_DIR)/wt-agent

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/wt-roomd: $(COMMON_OBJS) $(BUILD_DIR)/wt_http.o $(BUILD_DIR)/wt_roomd.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/wt-say: $(COMMON_OBJS) $(BUILD_DIR)/wt_say.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/wt-tail: $(COMMON_OBJS) $(BUILD_DIR)/wt_tail.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/wt-agent: $(COMMON_OBJS) $(BUILD_DIR)/wt_agent.o
	$(CC) $(LDFLAGS) -o $@ $^

run-roomd: $(BUILD_DIR)/wt-roomd
	./$(BUILD_DIR)/wt-roomd --config config/woventeam-phase0.conf

run-demo: all
	mkdir -p data
	./$(BUILD_DIR)/wt-say ceo all "Claude, ChatGPT, Gemini: each confirm that you can read and respond through the shared room."
	./$(BUILD_DIR)/wt-agent --agent claude --once
	./$(BUILD_DIR)/wt-agent --agent chatgpt --once
	./$(BUILD_DIR)/wt-agent --agent gemini --once
	./$(BUILD_DIR)/wt-tail --limit 20

test-smoke: all
	./tests/smoke-phase0-room.sh

test: test-smoke

install-roomd-service: all
	sudo install -m 0644 deploy/systemd/wt-roomd.service /etc/systemd/system/
	sudo systemctl daemon-reload
	sudo systemctl enable --now wt-roomd.service

clean:
	rm -rf $(BUILD_DIR)
