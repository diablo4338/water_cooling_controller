CC ?= cc
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter
LDFLAGS ?=
LDLIBS += -lm

CONTROL_DIR := ../../src/ble_protocol/control
CPPFLAGS += -Istubs -I$(CONTROL_DIR) -I../../src/ble_protocol/app

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
EVENT_BUS_TARGET := test_event_bus
INA226_TARGET := test_ina226
BLE_LIFECYCLE_TARGET := test_ble_lifecycle

.PHONY: all test clean

all: $(TARGET) $(EVENT_BUS_TARGET) $(BLE_LIFECYCLE_TARGET)

$(TARGET): $(SOURCES) $(wildcard $(CONTROL_DIR)/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

test: $(TARGET)
	./$(TARGET)
	$(MAKE) $(BLE_LIFECYCLE_TARGET)
	./$(BLE_LIFECYCLE_TARGET)
	$(MAKE) $(EVENT_BUS_TARGET)
	./$(EVENT_BUS_TARGET)
	$(MAKE) $(INA226_TARGET)
	./$(INA226_TARGET)

$(BLE_LIFECYCLE_TARGET): test_ble_lifecycle.c $(CONTROL_DIR)/runtime_events.c $(CONTROL_DIR)/load_fsm.c $(CONTROL_DIR)/buzzer_fsm.c $(CONTROL_DIR)/ble_fsm.c $(CONTROL_DIR)/power_manager.c $(CONTROL_DIR)/runtime_core.c $(wildcard $(CONTROL_DIR)/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(EVENT_BUS_TARGET): test_event_bus.c ../../src/ble_protocol/app/event_bus.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(INA226_TARGET): test_ina226.c ../../src/metrics/ina226.c
	$(CC) $(CPPFLAGS) -I../../src/metrics $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET) $(EVENT_BUS_TARGET) $(INA226_TARGET) $(BLE_LIFECYCLE_TARGET)
