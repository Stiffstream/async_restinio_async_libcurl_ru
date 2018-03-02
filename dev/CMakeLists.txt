cmake_minimum_required(VERSION 3.3)

SET(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-pthread -mtune=native -march=native)
set(CMAKE_EXE_LINKER_FLAGS "-pthread")
set(CMAKE_SHARED_LINKER_FLAGS "-pthread")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/asio/include)
add_definitions(-DASIO_STANDALONE -DASIO_HAS_STD_CHRONO -DASIO_DISABLE_STD_STRING_VIEW)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/fmt)
add_definitions(-DFMT_HEADER_ONLY)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/clara)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

find_package(CURL)
if (CURL_FOUND)
  include_directories( ${CURL_INCLUDE_DIRS} )
endif ()

add_subdirectory(nodejs/http_parser)

add_subdirectory(delay_server)
add_subdirectory(bridge_server_1)
add_subdirectory(bridge_server_1_pipe)
add_subdirectory(bridge_server_2)

