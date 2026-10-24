include(FetchContent)
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 2b24f5fa71d124de108da54d9da49ea3049dc91e # Or a specific version tag like v1.90.8
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
        imguizmo
        GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
        GIT_TAG eb90849e6f0e24279357026c689e5eb5cd15723b
)
FetchContent_MakeAvailable(imguizmo)

file(GLOB IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/*.cpp
        ${imguizmo_SOURCE_DIR}/*.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

add_library(imgui_lib STATIC
        ${IMGUI_SOURCES}
)

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui-fonts/
        ${imguizmo_SOURCE_DIR}
)

# Allow parent projects to specify ImGui user config
if(DEFINED IMGUI_USER_CONFIG_PATH)
    target_compile_definitions(imgui_lib PUBLIC
            "IMGUI_USER_CONFIG=\"${IMGUI_USER_CONFIG_PATH}\""
    )
endif()
