add_library(td_session_file STATIC)
init_non_host_target(td_session_file)
add_library(tdesktop::td_session_file ALIAS td_session_file)

target_sources(td_session_file PRIVATE
    ${src_loc}/main/session_file.cpp
    ${src_loc}/main/session_file.h
    ${src_loc}/main/session_sqlite.h
)
target_include_directories(td_session_file PUBLIC ${src_loc})
target_link_libraries(td_session_file PUBLIC Qt${QT_VERSION_MAJOR}::Core)

if (WIN32 AND NOT DESKTOP_APP_USE_PACKAGED)
    add_library(session_sqlite STATIC ${libs_loc}/tde2e/sqlite/sqlite/sqlite3.c)
    target_include_directories(session_sqlite PUBLIC ${libs_loc}/tde2e/sqlite/sqlite)
    target_compile_definitions(session_sqlite PRIVATE
        OMIT_MEMLOCK
        SQLITE_HAS_CODEC
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
    )
    target_link_libraries(session_sqlite PRIVATE desktop-app::external_openssl)
    target_compile_definitions(session_sqlite INTERFACE TDESKTOP_SESSION_TDSQLITE)
    target_link_libraries(td_session_file PRIVATE session_sqlite)
else()
    find_package(SQLite3 REQUIRED)
    if (TARGET SQLite3::SQLite3)
        set(session_sqlite_target SQLite3::SQLite3)
    else()
        set(session_sqlite_target SQLite::SQLite3)
    endif()
    target_link_libraries(td_session_file PRIVATE ${session_sqlite_target})
endif()
