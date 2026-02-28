set(SRCS
    "ads1115.c"
    "metrics.c")

if(DEFINED PAIR_RUN_TESTS)
    list(APPEND SRCS "test/test_ads1115.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES driver freertos log
    PRIV_REQUIRES unity
)

if(DEFINED PAIR_RUN_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PAIR_RUN_TESTS)
endif()
