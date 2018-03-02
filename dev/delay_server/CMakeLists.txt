set(TARGET delay_server)
set(TARGET_SRCFILES main.cpp)

add_executable(${TARGET} ${TARGET_SRCFILES})

target_link_libraries(${TARGET} nodejs_http_parser)

install(TARGETS ${TARGET} DESTINATION bin)
