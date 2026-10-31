CC ?= cc
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter
LDFLAGS ?=
LDLIBS += -lm

CONTROL_DIR := ../../src/ble_protocol/control
CPPFLAGS += -I$(CONTROL_DIR)

SOURCES := \
	test_runtime_fsm.c \
	$(CONTROL_DIR)/runtime_events.c \
	$(CONTROL_DIR)/load_fsm.c \
	$(CONTROL_DIR)/buzzer_fsm.c \
	$(CONTROL_DIR)/ble_fsm.c \
	$(CONTROL_DIR)/power_manager.c \
	$(CONTROL_DIR)/runtime_core.c \
	$(CONTROL_DIR)/load_state.c

TARGET := test_runtime_fsm

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(wildcard $(CONTROL_DIR)/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
