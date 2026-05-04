CC := gcc
PROJECT := MiniSync

BIN_DIR := bin
BUILD_DIR := build
SRC_DIR := src
INC_DIR := include

CFLAGS := -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I$(INC_DIR) -pthread -O2 -g
LDFLAGS := -pthread

CLIENT_TARGET := $(BIN_DIR)/minisync_client
SERVER_TARGET := $(BIN_DIR)/minisync_server

COMMON_SRCS := \
	$(SRC_DIR)/logger.c \
	$(SRC_DIR)/hash.c \
	$(SRC_DIR)/file_utils.c \
	$(SRC_DIR)/net_utils.c

CLIENT_SRCS := \
	$(SRC_DIR)/client_main.c \
	$(SRC_DIR)/monitor.c \
	$(SRC_DIR)/transfer.c \
	$(SRC_DIR)/task_queue.c \
	$(COMMON_SRCS)

SERVER_SRCS := \
	$(SRC_DIR)/server_main.c \
	$(SRC_DIR)/server.c \
	$(COMMON_SRCS)

CLIENT_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/client/%.o,$(CLIENT_SRCS))
SERVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/server/%.o,$(SERVER_SRCS))

.PHONY: all clean dirs run-client-local run-server

all: dirs $(CLIENT_TARGET) $(SERVER_TARGET)

dirs:
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)/client $(BUILD_DIR)/server logs test_dir backup_dir server_backup

$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@ $(LDFLAGS)

$(SERVER_TARGET): $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/client/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/server/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run-client-local: all
	./$(CLIENT_TARGET) --mode local --src ./test_dir --dst ./backup_dir

run-server: all
	./$(SERVER_TARGET) --port 9000 --root ./server_backup

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
