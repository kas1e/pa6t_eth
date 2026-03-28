CC = ppc-amigaos-gcc

INCLUDES = -I./include \
           -I/usr/local/amiga/ppc-amigaos/SDK/include/include_h \
           -I/usr/local/amiga/ppc-amigaos/SDK/include/include_h/interfaces

ifdef DEBUG
CFLAGS  = -O2 -Wall -gstabs -DDEBUG=$(DEBUG) $(INCLUDES) -fno-tree-loop-distribute-patterns -MMD -MP
else
CFLAGS  = -O2 -Wall -DNODEBUG $(INCLUDES) -fno-tree-loop-distribute-patterns -MMD -MP
endif

LDFLAGS = -nostartfiles

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/pa6t_eth.device

SRC = src/device.c \
      src/Init.c \
      src/Open.c \
      src/Close.c \
      src/Expunge.c \
      src/BeginIO.c \
      src/unit_task.c \
      src/hw/pci.c \
      src/hw/hw_init.c \
      src/hw/irq.c \
      src/hw/rx.c \
      src/hw/tx.c

OBJ = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJ:.o=.d)

.PHONY: all clean
