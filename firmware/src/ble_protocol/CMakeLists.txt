set(SRCS
    "app/main.c"
    "app/app_tasks.c"
    "app/action_executor.c"
    "app/event_bus.c"
    "app/load_runtime.c"
    "control/load_state.c"
    "control/load_fsm.c"
    "control/buzzer_fsm.c"
    "control/buzzer_logic.c"
    "control/ble_fsm.c"
    "control/power_manager.c"
    "control/runtime_core.c"
    "control/runtime_events.c"
    "control/runtime_config.c"
    "hw/load_output.c"
    "ble/access.c"
    "ble/load_metrics_ble.c"
    "ble/gap.c"
    "ble/gatt.c"
    "ble/uuid.c"
    "pairing/conn_guard.c"
    "pairing/crypto.c"
    "pairing/ecdh.c"
    "pairing/host_verify.c"
    "pairing/pair_mode.c"
    "pairing/pair_state.c"
    "pairing/state.c"
    "pairing/storage.c")

if(DEFINED PAIR_RUN_TESTS)
    list(APPEND SRCS "../../tests/ble_protocol/test_buzzer_logic.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_runtime_config.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_load_state.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_connect_window.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_pair_state.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_conn_guard.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_fsm.c")
    list(APPEND SRCS "../../tests/ble_protocol/test_storage.c")
endif()

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "." "app" "ble" "control" "pairing" "hw"
                       REQUIRES bt mbedtls nvs_flash driver esp_timer i2c_bus log freertos metrics
                       PRIV_REQUIRES unity)

# Keep lifecycle diagnostics available even when the global default is WARN.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)

if(DEFINED PAIR_RUN_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAIR_RUN_TESTS)
endif()
