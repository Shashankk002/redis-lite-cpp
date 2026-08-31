add_executable(redis-lite-server main.cpp)

target_link_libraries(redis-lite-server
    PRIVATE
        redis_lite_headers
        redis_lite_warnings)
