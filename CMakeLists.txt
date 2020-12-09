cmake_minimum_required(VERSION 3.0.0)
project(server VERSION 0.1.0)

include(CTest)
enable_testing()

add_executable(server server1.c)
target_link_libraries(server pthread)

# add_executable(server test.c)
# target_link_libraries(server pthread)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
