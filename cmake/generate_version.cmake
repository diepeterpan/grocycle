cmake_minimum_required(VERSION 3.20)

set(BUILD_NUMBER_FILE "${CMAKE_CURRENT_LIST_DIR}/.build_number")
set(BUILD_NUMBER 1)

if(EXISTS "${BUILD_NUMBER_FILE}")
    file(READ "${BUILD_NUMBER_FILE}" CURRENT_BUILD_NUMBER)
    string(STRIP "${CURRENT_BUILD_NUMBER}" CURRENT_BUILD_NUMBER)
    if(CURRENT_BUILD_NUMBER MATCHES "^[0-9]+$")
        math(EXPR BUILD_NUMBER "${CURRENT_BUILD_NUMBER} + 1")
    endif()
endif()

file(WRITE "${BUILD_NUMBER_FILE}" "${BUILD_NUMBER}\n")

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

configure_file(
    "${ROOT_SOURCE_DIR}/include/common/version.hpp.in"
    "${OUTPUT_HEADER}"
    @ONLY
)

message(STATUS "Configured build #${BUILD_NUMBER} (${BUILD_TIMESTAMP})")
