cmake_minimum_required(VERSION 3.30.0)

project(pdfsearch VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-warn-execstack")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-warn-execstack")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-warn-execstack")

find_package(PkgConfig REQUIRED)

# This should pull in most dependencies automatically
pkg_check_modules(POPPLER_GLIB REQUIRED poppler-glib)

add_executable(${PROJECT_NAME} pdfsearch.c routes.c cli.c)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    pgpool 
    solidc 
    pulsar 
    cjson 
    pq 
    m
    ${POPPLER_GLIB_LIBRARIES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${POPPLER_GLIB_INCLUDE_DIRS}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${POPPLER_GLIB_CFLAGS_OTHER}
)
