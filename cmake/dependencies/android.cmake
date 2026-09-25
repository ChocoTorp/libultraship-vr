include(FetchContent)

#=================== SDL2 ===================
find_package(SDL2 QUIET)
if (NOT ${SDL2_FOUND})
    FetchContent_Declare(
        SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    )
    message("SDL2 not found. Downloading now...")
    FetchContent_MakeAvailable(SDL2)
    message("SDL2 downloaded to " ${FETCHCONTENT_BASE_DIR}/sdl2-src)
endif()

#=================== nlohmann-json ===================
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#=================== tinyxml2 ===================
find_package(tinyxml2 QUIET)
if (NOT ${tinyxml2_FOUND})
    set(tinyxml2_BUILD_TESTING OFF)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 11.0.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(tinyxml2)
endif()

#=================== spdlog ===================
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

#=================== libzip ===================
find_package(libzip QUIET)
if (NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
        libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.11.4
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

target_link_libraries(ImGui PUBLIC SDL2::SDL2)

#=================== glm (QuestShip: header-only math for the VR layer) ===================
# SOURCE_SUBDIR points nowhere so MakeAvailable only downloads (no glm targets/tests are built).
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    SOURCE_SUBDIR questship-headers-only
)
FetchContent_MakeAvailable(glm)
list(APPEND ADDITIONAL_LIB_INCLUDES ${glm_SOURCE_DIR})

#=================== astc-encoder (QuestShip: on-device texture-pack optimizer) ===================
# Arm's ASTC compressor as a static library (NEON), used by TexturePackOptimizer to convert
# installed HD texture packs to ASTC in the background.
set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    astcenc
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG 5.7.0
)
FetchContent_MakeAvailable(astcenc)
list(APPEND ADDITIONAL_LIB_INCLUDES ${astcenc_SOURCE_DIR}/Source)
