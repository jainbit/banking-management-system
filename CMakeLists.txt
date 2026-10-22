cmake_minimum_required(VERSION 3.10)
project(BankingSystem C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)          # gnu11, same as the Makefile
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)

add_executable(server
        server.c
        signals.c
        auth.c
        records.c
        customer.c
        employee.c
        manager.c
        admin.c
        utils.c
        session_manager.c
)
target_link_libraries(server Threads::Threads)

add_executable(client
        client.c
)
