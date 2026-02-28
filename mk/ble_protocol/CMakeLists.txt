set(SRCS
    "main.c"
    "app_tasks.c"
    "access.c"
    "conn_guard.c"
    "crypto.c"
    "ecdh.c"
    "gatt.c"
    "gap.c"
    "host_verify.c"
    "rgb.c"
    "pair_state.c"
    "state.c"
    "storage.c"
    "uuid.c")

if(DEFINED PAIR_RUN_TESTS)
    list(APPEND SRCS "test/test_pair_state.c")
    list(APPEND SRCS "test/test_conn_guard.c")
    list(APPEND SRCS "test/test_fsm.c")
endif()

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES bt mbedtls nvs_flash driver esp_timer log freertos esp_driver_rmt
                       PRIV_REQUIRES unity)

if(DEFINED PAIR_RUN_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAIR_RUN_TESTS)
endif()
