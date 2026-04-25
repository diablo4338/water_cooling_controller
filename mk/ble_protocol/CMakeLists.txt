set(SRCS
    "app/main.c"
    "app/app_tasks.c"
    "ble/access.c"
    "ble/device_status_ble.c"
    "ble/fan_status_ble.c"
    "ble/gap.c"
    "ble/gatt.c"
    "ble/metrics_ble.c"
    "ble/operation_status_ble.c"
    "ble/uuid.c"
    "control/device_status.c"
    "control/fan_control.c"
    "control/operation_manager.c"
    "control/params.c"
    "hw/status_led.c"
    "pairing/conn_guard.c"
    "pairing/crypto.c"
    "pairing/ecdh.c"
    "pairing/host_verify.c"
    "pairing/pair_mode.c"
    "pairing/pair_state.c"
    "pairing/state.c"
    "pairing/storage.c")

if(DEFINED PAIR_RUN_TESTS)
    list(APPEND SRCS "test/test_pair_state.c")
    list(APPEND SRCS "test/test_conn_guard.c")
    list(APPEND SRCS "test/test_fsm.c")
    list(APPEND SRCS "test/test_storage.c")
    list(APPEND SRCS "test/test_status_led.c")
endif()

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "." "app" "ble" "control" "pairing" "hw"
                       REQUIRES bt mbedtls nvs_flash driver esp_timer log freertos metrics
                       PRIV_REQUIRES unity)

if(DEFINED PAIR_RUN_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAIR_RUN_TESTS)
endif()
