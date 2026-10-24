set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui")

file(GLOB IMGUI_SOURCES
        "${IMGUI_DIR}/*.cpp"
)

#target_compile_definitions(imgui PUBLIC
#        "IMGUI_USER_CONFIG=\"${CMAKE_CURRENT_SOURCE_DIR}/sf2-imgui/sf2_imconfig.hpp\""
#)

add_library(imgui STATIC ${IMGUI_SOURCES})

#target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
