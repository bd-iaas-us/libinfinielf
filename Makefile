CC = gcc
CFLAGS = -g -Wall -O2 -Iinclude
LDFLAGS = -libverbs -luv
ifeq ($(HAVE_CUDA),1)
	CFLAGS += -I/usr/local/cuda/include -DHAVE_CUDA
	LDFLAGS += -L/usr/local/cuda/lib64 -lcuda -lcudart
endif

LIB_NAME = libinfelf.so
SRC_DIR = src
LIB_DIR = lib
BUILD_DIR = build
SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
OBJ_FILES = $(SRC_FILES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

EXAMPLE = example
EXAMPLE_SRCS = $(wildcard example/*.c)
EXAMPLE_OBJS = $(EXAMPLE_SRCS:example/%.c=$(BUILD_DIR)/%.o)

# Targets
all: $(LIB_DIR)/$(LIB_NAME) $(EXAMPLE)

$(LIB_DIR)/$(LIB_NAME): $(OBJ_FILES)
	@mkdir -p $(LIB_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(EXAMPLE): $(LIB_DIR)/$(LIB_NAME)
	$(CC) $(CFLAGS) -c example/client.c -o $(BUILD_DIR)/client.o
	$(CC) $(CFLAGS) -c example/server.c -o $(BUILD_DIR)/server.o
	$(CC) $(CFLAGS) -c example/example.c -o $(BUILD_DIR)/example.o
	$(CC) -o $(BUILD_DIR)/example $(EXAMPLE_OBJS) -L$(LIB_DIR) $(LDFLAGS) -linfelf

clean:
	rm -rf $(BUILD_DIR)/*.o $(BUILD_DIR)/$(EXAMPLE)

.PHONY: all clean