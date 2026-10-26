cmake_minimum_required(VERSION 3.20)
project(grocycle VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE language servers and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2 -pthread)
endif()

# Find SQLite3
find_package(SQLite3 REQUIRED)

# Fetch Dependencies using FetchContent
include(FetchContent)

# 1. FTXUI
message(STATUS "Fetching FTXUI...")
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "Disable FTXUI installation rules" FORCE)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

# 2. CLI11
message(STATUS "Fetching CLI11...")
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cli11)

# 3. nlohmann_json
message(STATUS "Fetching nlohmann_json...")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Include directories
include_directories(include ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Generate version.hpp on build
add_custom_target(generate_version_header
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DROOT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT_HEADER=${CMAKE_CURRENT_BINARY_DIR}/generated/common/version.hpp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_version.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/common/version.hpp
    COMMENT "Updating Grocycle build number and compilation timestamp..."
)

# Common library (shared between daemon and CLI)
add_library(grocycle_core STATIC
    src/common/config.cpp
    src/common/utils.cpp
    src/db/storage_engine.cpp
    src/ipc/unix_socket.cpp
    src/scanner/fs_scanner.cpp
    src/scanner/throttle.cpp
)
add_dependencies(grocycle_core generate_version_header)


target_link_libraries(grocycle_core PUBLIC
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    pthread
)

# Background Daemon executable: grocycled
add_executable(grocycled
    src/daemon/main_daemon.cpp
)
target_link_libraries(grocycled PRIVATE
    grocycle_core
    CLI11::CLI11
)

# CLI and TUI executable: grocycle
add_executable(grocycle
    src/cli/main_cli.cpp
    src/cli/cli_commands.cpp
    src/cli/tui_app.cpp
)
target_link_libraries(grocycle PRIVATE
    grocycle_core
    CLI11::CLI11
    ftxui::screen
    ftxui::dom
    ftxui::component
)

# Test executables
enable_testing()

add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE grocycle_core)
add_test(NAME test_storage COMMAND test_storage)

add_executable(test_scanner tests/test_scanner.cpp)
target_link_libraries(test_scanner PRIVATE grocycle_core)
add_test(NAME test_scanner COMMAND test_scanner)

add_executable(test_growth tests/test_growth.cpp)
target_link_libraries(test_growth PRIVATE grocycle_core)
add_test(NAME test_growth COMMAND test_growth)

# ==============================================================================
# Quality Assurance: Linting & Code Formatting
# ==============================================================================
set(GROCYCLE_SOURCES
    src/common/config.cpp
    src/common/utils.cpp
    src/db/storage_engine.cpp
    src/ipc/unix_socket.cpp
    src/scanner/fs_scanner.cpp
    src/scanner/throttle.cpp
    src/daemon/main_daemon.cpp
    src/cli/main_cli.cpp
    src/cli/cli_commands.cpp
    src/cli/tui_app.cpp
)

set(GROCYCLE_ALL_FILES
    ${GROCYCLE_SOURCES}
    include/common/config.hpp
    include/common/utils.hpp
    include/db/storage_engine.hpp
    include/ipc/unix_socket.hpp
    include/scanner/fs_scanner.hpp
    include/scanner/throttle.hpp
    include/cli/cli_commands.hpp
    include/tui/tui_app.hpp
)

find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE)
    add_custom_target(lint
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            ${GROCYCLE_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy static analysis on Grocycle sources..."
        VERBATIM
    )
    add_dependencies(lint generate_version_header)
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${GROCYCLE_ALL_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting Grocycle sources with clang-format..."
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${GROCYCLE_ALL_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking Grocycle code formatting..."
        VERBATIM
    )
endif()

# ==============================================================================
# Installation & Packaging (CPack)
# ==============================================================================
include(GNUInstallDirs)

# Install binaries
install(TARGETS grocycle grocycled
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Application
)

# Install systemd service
install(FILES configs/grocycle.service
    DESTINATION /lib/systemd/system
    COMPONENT SystemService
)

# Install example configuration
install(FILES configs/grocycle.conf.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/grocycle
    RENAME config.json.example
    COMPONENT Configuration
)

# CPack configuration
set(CPACK_PACKAGE_NAME "grocycle")
set(CPACK_PACKAGE_VENDOR "Grocycle Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Linux directory growth monitor and interactive TUI explorer")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "grocycle@local")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_amd64")

# Debian (.deb) specific settings
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Grocycle Maintainers <grocycle@local>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.34), libsqlite3-0 (>= 3.35)")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
)

include(CPack)
