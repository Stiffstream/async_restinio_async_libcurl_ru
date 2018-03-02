set(TARGET bridge_server_1)
set(TARGET_SRCFILES main.cpp)

add_executable(${TARGET} ${TARGET_SRCFILES})

target_link_libraries(${TARGET} nodejs_http_parser ${CURL_LIBRARIES})

install(TARGETS ${TARGET} DESTINATION bin)
