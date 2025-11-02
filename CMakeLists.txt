cmake_minimum_required(VERSION 3.30.0)

project(lexicon VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Necessary to avoid warning since we use defer that relies on __attribute__(cleanup)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-warn-execstack")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-warn-execstack")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-warn-execstack")

find_package(PkgConfig REQUIRED)
find_package(PostgreSQL REQUIRED)

# This should pull in most dependencies automatically
pkg_check_modules(POPPLER_GLIB REQUIRED poppler-glib)

# The executable needs ./ui/dist svelte frontend served at / route.
# Build with: `cd ui` && `npm run build`
add_executable(${PROJECT_NAME} 
    lexicon.c 
    routes.c 
    cli.c 
    cache.c 
    logger.c 
    pdf.c 
    pdf_preprocess.c
)

# All dependencies from abiiranathan can be built from source with cmake
target_link_libraries(${PROJECT_NAME} PRIVATE 
    pgconn # github.com/abiiranathan/pgconn
    solidc # github.com/abiiranathan/solidc
    pulsar # github.com/abiiranathan/pulsar
    yyjson # https://github.com/ibireme/yyjson
    m      # math library
    z      # zlib
    ${POPPLER_GLIB_LIBRARIES}
    ${PostgreSQL_LIBRARIES}
    gio-2.0
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${POPPLER_GLIB_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${POPPLER_GLIB_CFLAGS_OTHER}
)
