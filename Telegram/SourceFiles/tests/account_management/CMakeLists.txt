cmake_minimum_required(VERSION 3.20)
project(account_management_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(account_management_tests
    ../test_account_management.cpp
    ../../main/account_management.cpp)
target_include_directories(account_management_tests PRIVATE ../..)
target_link_libraries(account_management_tests PRIVATE Qt6::Core)
enable_testing()
add_test(NAME account_management COMMAND account_management_tests)
