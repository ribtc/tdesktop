cmake_minimum_required(VERSION 3.24)
project(account_consumer_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)
get_filename_component(source_files "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(checkout "${source_files}/../.." ABSOLUTE)
set(TELEGRAM_TOOLKIT_ROOT "${checkout}" CACHE PATH "Checkout with toolkit submodules")
add_executable(test_account_consumers
    ../test_account_consumers.cpp
    ../network_context/assertion_log.cpp
    ../../core/websocket_client.cpp
    ../../mtproto/network_context.cpp
    ../../mtproto/mtproto_proxy_data.cpp
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base/base/random.cpp
)
target_include_directories(test_account_consumers PRIVATE
    ${source_files}
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_crl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_rpl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/ThirdParty/GSL/include
)
target_precompile_headers(test_account_consumers PRIVATE ../../mtproto/network_context.h)
target_link_libraries(test_account_consumers PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
enable_testing()
add_test(NAME account_consumers COMMAND test_account_consumers)
if (UNIX)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(test_websocket_capacity
        ../test_websocket_capacity.cpp
        ../network_context/assertion_log.cpp
        ../../core/websocket_client.cpp
        ../../mtproto/network_context.cpp
        ../../mtproto/mtproto_proxy_data.cpp
        ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base/base/random.cpp
    )
    get_target_property(test_includes test_account_consumers INCLUDE_DIRECTORIES)
    target_include_directories(test_websocket_capacity PRIVATE ${test_includes})
    target_precompile_headers(test_websocket_capacity REUSE_FROM test_account_consumers)
    target_link_libraries(test_websocket_capacity PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
    if (APPLE)
        find_package(Qt6 REQUIRED COMPONENTS Gui)
        target_link_libraries(test_websocket_capacity PRIVATE Qt6::Gui)
    endif()
    add_test(NAME websocket_capacity COMMAND test_websocket_capacity
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/websocket_server.py)
    set_tests_properties(websocket_capacity PROPERTIES TIMEOUT 30)
endif()
