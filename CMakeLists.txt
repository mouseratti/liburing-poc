cmake_minimum_required(VERSION 3.20)
project(liburing_prototype)

set(CMAKE_CXX_STANDARD 14)
add_compile_options(-fpermissive)
find_package(PkgConfig)
pkg_check_modules(LIBURING REQUIRED liburing)

include_directories(${LIBURING_INCLUDE_DIRS})


#add_executable(liburing_prototype
#        src/webserver.cpp)
#add_executable(probing
#        src/probing.cpp)
#target_link_libraries(probing uring)

add_executable(echo src/echo.cpp)
target_link_libraries(echo uring)
add_executable(echo1 src/echo1.cpp)
target_link_libraries(echo1 uring)
add_executable(echo2 src/echo2.cpp)
target_link_libraries(echo2 uring)