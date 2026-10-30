cmake_minimum_required(VERSION 3.24)
project(account_network_context_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)

get_filename_component(source_files "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(checkout "${source_files}/../.." ABSOLUTE)
set(TELEGRAM_TOOLKIT_ROOT "${checkout}" CACHE PATH "Checkout with toolkit submodules")

add_executable(test_network_context
    ../test_network_context.cpp
    assertion_log.cpp
    ../../core/proxy_library.cpp
    ../../mtproto/http_transport.cpp
    ../../mtproto/network_context.cpp
    ../../mtproto/mtproto_proxy_data.cpp
)
target_include_directories(test_network_context PRIVATE
    ${source_files}
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_crl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_rpl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/ThirdParty/GSL/include
)
target_precompile_headers(test_network_context PRIVATE
    ../../mtproto/network_context.h
)
target_link_libraries(test_network_context PRIVATE Qt6::Core Qt6::Network)
enable_testing()
add_test(NAME account_network_context COMMAND test_network_context)

add_executable(test_proxy_import
    ../test_proxy_import.cpp
    assertion_log.cpp
    ../../core/proxy_import.cpp
    ../../core/proxy_check_queue.cpp
    ../../core/proxy_library.cpp
    ../../mtproto/mtproto_proxy_data.cpp
)
target_include_directories(test_proxy_import PRIVATE
    ${source_files}
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_crl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_rpl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/ThirdParty/GSL/include
)
target_precompile_headers(test_proxy_import PRIVATE ../../mtproto/network_context.h)
target_link_libraries(test_proxy_import PRIVATE Qt6::Core Qt6::Network)
add_test(NAME proxy_import COMMAND test_proxy_import)

add_executable(test_calls_network
    ../test_calls_network.cpp
    assertion_log.cpp
    ../../mtproto/network_context.cpp
    ../../mtproto/mtproto_proxy_data.cpp
)
target_include_directories(test_calls_network PRIVATE
    ${source_files}
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_base
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_crl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/lib_rpl
    ${TELEGRAM_TOOLKIT_ROOT}/Telegram/ThirdParty/GSL/include
)
target_precompile_headers(test_calls_network PRIVATE
    ../../mtproto/network_context.h
)
target_link_libraries(test_calls_network PRIVATE Qt6::Core Qt6::Network)
add_test(NAME calls_network COMMAND test_calls_network)
