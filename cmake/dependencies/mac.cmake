include(FetchContent)

# macOS: a stale/partial /Library/Frameworks/SDL2.framework can win find_package(SDL2)
# over Homebrew's and break the build (SDL2_INCLUDE_DIR -> missing /Library/Headers).
# Search frameworks last and make sure the Homebrew prefix is on CMAKE_PREFIX_PATH.
set(CMAKE_FIND_FRAMEWORK LAST)
find_program(BREW_EXECUTABLE brew)
if (BREW_EXECUTABLE)
    execute_process(COMMAND ${BREW_EXECUTABLE} --prefix
                    OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (BREW_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${BREW_PREFIX}")
    endif()
endif()

#=================== spdlog ===================
# macports has issues with this because of fmt
# brew doesn't support building multiarch
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

#=================== Metal-cpp ===================
FetchContent_Declare(
    metalcpp
    GIT_REPOSITORY https://github.com/briaguya-ai/single-header-metal-cpp.git
    GIT_TAG macOS13_iOS16
)
FetchContent_MakeAvailable(metalcpp)
list(APPEND ADDITIONAL_LIB_INCLUDES ${metalcpp_SOURCE_DIR})

#=================== ImGui ===================
target_sources(ImGui
    PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
)

target_include_directories(ImGui PRIVATE ${metalcpp_SOURCE_DIR})
target_compile_definitions(ImGui PUBLIC IMGUI_IMPL_METAL_CPP)

find_package(SDL2 REQUIRED)
target_link_libraries(ImGui PUBLIC SDL2::SDL2)

find_package(GLEW REQUIRED)
target_link_libraries(ImGui PUBLIC ${OPENGL_opengl_LIBRARY} GLEW::GLEW)
set_target_properties(ImGui PROPERTIES
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
)
