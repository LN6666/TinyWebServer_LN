cmake_minimum_required(VERSION 3.22)
project(Tinywebserver)

set(CMAKE_CXX_STANDARD 14)

add_executable(Tinywebserver main.cpp block_queue.h http_conn.h http_conn.cpp lst_timer.h locker.h log.cpp log.h sql_connection_pool.cpp sql_connection_pool.h threadpool.h)
