CC ?= cc
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG
LDFLAGS ?=
LDLIBS += -lm

CONTROL_DIR := ../../src/ble_protocol/control
CPPFLAGS += -Istubs -I$(CONTROL_DIR) -I../../src/ble_protocol/app -I../../src/ble_protocol/ble -I../../src/ble_protocol/hw -I../../src/metrics

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
ACTION_EXECUTOR_TARGET := test_action_executor
REED_DETECTOR_TARGET := test_reed_detector
LOAD_CHARGING_TARGET := test_load_charging
RUNTIME_CONFIG_TARGET := test_runtime_config

.PHONY: all test clean

all: $(TARGET) $(EVENT_BUS_TARGET) $(BLE_LIFECYCLE_TARGET) $(ACTION_EXECUTOR_TARGET) $(REED_DETECTOR_TARGET) $(LOAD_CHARGING_TARGET) $(RUNTIME_CONFIG_TARGET)

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
	$(MAKE) $(ACTION_EXECUTOR_TARGET)
	./$(ACTION_EXECUTOR_TARGET)
	$(MAKE) $(REED_DETECTOR_TARGET)
	./$(REED_DETECTOR_TARGET)
	$(MAKE) $(LOAD_CHARGING_TARGET)
	./$(LOAD_CHARGING_TARGET)
	$(MAKE) $(RUNTIME_CONFIG_TARGET)
	./$(RUNTIME_CONFIG_TARGET)

$(REED_DETECTOR_TARGET): test_reed_detector.c $(CONTROL_DIR)/reed_detector.c $(CONTROL_DIR)/reed_detector.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_reed_detector.c $(CONTROL_DIR)/reed_detector.c $(LDLIBS)

$(LOAD_CHARGING_TARGET): test_load_charging.c $(CONTROL_DIR)/runtime_events.c $(CONTROL_DIR)/load_fsm.c $(CONTROL_DIR)/power_manager.c $(wildcard $(CONTROL_DIR)/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_load_charging.c $(CONTROL_DIR)/runtime_events.c $(CONTROL_DIR)/load_fsm.c $(CONTROL_DIR)/power_manager.c $(LDLIBS)

$(RUNTIME_CONFIG_TARGET): test_runtime_config.c $(CONTROL_DIR)/runtime_config.c $(CONTROL_DIR)/runtime_config.h stubs/nvs.h stubs/freertos/portmacro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_runtime_config.c $(CONTROL_DIR)/runtime_config.c $(LDLIBS)

$(BLE_LIFECYCLE_TARGET): test_ble_lifecycle.c $(CONTROL_DIR)/runtime_events.c $(CONTROL_DIR)/load_fsm.c $(CONTROL_DIR)/buzzer_fsm.c $(CONTROL_DIR)/ble_fsm.c $(CONTROL_DIR)/power_manager.c $(CONTROL_DIR)/runtime_core.c $(wildcard $(CONTROL_DIR)/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_ble_lifecycle.c $(CONTROL_DIR)/runtime_events.c $(CONTROL_DIR)/load_fsm.c $(CONTROL_DIR)/buzzer_fsm.c $(CONTROL_DIR)/ble_fsm.c $(CONTROL_DIR)/power_manager.c $(CONTROL_DIR)/runtime_core.c $(LDLIBS)

$(EVENT_BUS_TARGET): test_event_bus.c ../../src/ble_protocol/app/event_bus.c $(CONTROL_DIR)/power_manager.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(INA226_TARGET): test_ina226.c ../../src/metrics/ina226.c
	$(CC) $(CPPFLAGS) -I../../src/metrics $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ACTION_EXECUTOR_TARGET): test_action_executor.c ../../src/ble_protocol/app/action_executor.c ../../src/ble_protocol/hw/load_output.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_action_executor.c ../../src/ble_protocol/hw/load_output.c $(LDLIBS)

clean:
	rm -f $(TARGET) $(EVENT_BUS_TARGET) $(INA226_TARGET) $(BLE_LIFECYCLE_TARGET) $(ACTION_EXECUTOR_TARGET) $(REED_DETECTOR_TARGET) $(LOAD_CHARGING_TARGET) $(RUNTIME_CONFIG_TARGET)
