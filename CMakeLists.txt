cmake_minimum_required(VERSION 3.20)

project(lexicon VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(WORKER_COUNT "4" CACHE STRING "Number of PDF indexer worker threads")
set(SERVER_WORKERS "2" CACHE STRING "Number of server worker threads")

find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(JPEG REQUIRED)

message(STATUS "JPEG library: ${JPEG_LIBRARIES}")
message(STATUS "JPEG include: ${JPEG_INCLUDE_DIRS}")

# This should pull in most dependencies automatically
pkg_check_modules(POPPLER_GLIB REQUIRED poppler-glib)

# The executable needs ./ui/dist svelte frontend served at / route.
# Build with: `cd ui` && `npm run build`
add_executable(${PROJECT_NAME} 
    src/main.c 
    src/routes.c 
    src/pdf_indexer.c
    src/pdf.c 
    src/pdf_preprocess.c
    src/database.c
    src/json_response.c
    src/json_allocator.c
)

# All dependencies from abiiranathan can be built from source with cmake
target_link_libraries(${PROJECT_NAME} PRIVATE 
    pgpool # github.com/abiiranathan/pgpool     (libpq connection pool library)
    solidc # github.com/abiiranathan/solidc     (Cross-platform C utilities and DS)
    vfs # github.com/abiiranathan/vfs          (Virtual File System)
    pulsar # github.com/abiiranathan/pulsar     (Robust epoll-based web server)
    yyjson # https://github.com/ibireme/yyjson  (Json serializer and parser)
    m      # math library
    z      # zlib
    ${POPPLER_GLIB_LIBRARIES}
    ${PostgreSQL_LIBRARIES}
    JPEG::JPEG
    gio-2.0
    pcre2-8
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${POPPLER_GLIB_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE 
    _GNU_SOURCE 
    WORKER_COUNT=${WORKER_COUNT}
    WORKERS=${SERVER_WORKERS}
)

# Enable LTO for Release builds
if(CMAKE_BUILD_TYPE MATCHES "Release")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-flto)
        add_link_options(-flto)
    endif()
endif()
