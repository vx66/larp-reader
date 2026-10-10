cmake_minimum_required(VERSION 3.20)

project(LarpReader
    VERSION 0.0.10
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(TINYXML2 REQUIRED tinyxml2)

foreach(flag_var
    SDL2_CFLAGS_OTHER
    SDL2_LDFLAGS_OTHER

    SDL2_IMAGE_CFLAGS_OTHER
    SDL2_IMAGE_LDFLAGS_OTHER

    SDL2_TTF_CFLAGS_OTHER
    SDL2_TTF_LDFLAGS_OTHER

    TINYXML2_CFLAGS_OTHER
    TINYXML2_LDFLAGS_OTHER
)
    list(REMOVE_ITEM ${flag_var} "-pthread")
endforeach()

set(PSP_LIB_DIR "$ENV{PSPDEV}/psp/lib")

find_library(LIBZIP_LIBRARY
    NAMES zip
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(MBEDTLS_LIBRARY
    NAMES mbedtls
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(MBEDX509_LIBRARY
    NAMES mbedx509
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(MBEDCRYPTO_LIBRARY
    NAMES mbedcrypto
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(BZIP2_LIBRARY
    NAMES bz2
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(LZMA_LIBRARY
    NAMES lzma
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(ZLIB_LIBRARY
    NAMES z
    PATHS "${PSP_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

add_executable(larpreader
    src/main.cpp
)

target_include_directories(larpreader PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_IMAGE_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${TINYXML2_INCLUDE_DIRS}
)

target_compile_options(larpreader PRIVATE
    -Wall
    -Wextra

    ${SDL2_CFLAGS_OTHER}
    ${SDL2_IMAGE_CFLAGS_OTHER}
    ${SDL2_TTF_CFLAGS_OTHER}
    ${TINYXML2_CFLAGS_OTHER}
)

target_link_options(larpreader PRIVATE
    ${SDL2_LDFLAGS_OTHER}
    ${SDL2_IMAGE_LDFLAGS_OTHER}
    ${SDL2_TTF_LDFLAGS_OTHER}
    ${TINYXML2_LDFLAGS_OTHER}
)

target_link_libraries(larpreader PRIVATE
    ${SDL2_TTF_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
    ${SDL2_LIBRARIES}

    ${LIBZIP_LIBRARY}
    ${TINYXML2_LIBRARIES}

    ${MBEDTLS_LIBRARY}
    ${MBEDX509_LIBRARY}
    ${MBEDCRYPTO_LIBRARY}
    ${BZIP2_LIBRARY}
    ${LZMA_LIBRARY}
    ${ZLIB_LIBRARY}
)

create_pbp_file(
    TARGET larpreader
    TITLE "Larp Reader"
    VERSION "00.10"
    MEMSIZE 1
)
