add_executable(redis_lite_tests
    test_main.cpp
    test_smoke.cpp)

target_include_directories(redis_lite_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(redis_lite_tests
    PRIVATE
        redis_lite_headers
        redis_lite_warnings)

add_test(NAME redis_lite_tests COMMAND redis_lite_tests)
