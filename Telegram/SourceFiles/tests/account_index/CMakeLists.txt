cmake_minimum_required(VERSION 3.20)
project(account_index_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(account_index_tests
    ../test_account_index.cpp
    ../../storage/storage_account_index.cpp)
target_include_directories(account_index_tests PRIVATE ../..)
target_link_libraries(account_index_tests PRIVATE Qt6::Core)
enable_testing()
add_test(NAME account_index COMMAND account_index_tests)
