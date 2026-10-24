cmake_minimum_required(VERSION 3.27)

project(
  vr_framework
  VERSION 1.0.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
#set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

include(GNUInstallDirs)

# Streamline version configuration
set(STREAMLINE_VERSION "v2.4.15" CACHE STRING "Version of NVIDIA Streamline to use")

option(INVERT_DEPTH_IN_SHADER "Invert depth in motion vector correction shader" OFF)

#add_compile_definitions(GLM_FORCE_LEFT_HANDED)

# CPM
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
set(CPM_SOURCE_CACHE ${CMAKE_CURRENT_BINARY_DIR}/.cpm)

# Dependencies
#CPMAddPackage("gh:brofield/simpleini@4.20")
#CPMAddPackage("gh:stevemk14ebr/PolyHook_2_0#master")
# Target: minhook
include("cmake/spdlog.cmake")
include("cmake/minhook.cmake")
include("cmake/glm.cmake")
#include("cmake/vigemclient.cmake")
include("cmake/safetyhook.cmake")
#include("cmake/polyhook.cmake")
include("cmake/kananlib.cmake")
include("cmake/directxtk.cmake")
include("cmake/openxr.cmake")
include("cmake/streamline.cmake")
include("cmake/imgui.cmake")
include("cmake/fidelityfx.cmake")


# Add headers
file(
  GLOB_RECURSE
  headers
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

list(REMOVE_ITEM headers ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h)


# Add sources
file(
  GLOB_RECURSE
  sources
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)


add_library(${PROJECT_NAME} STATIC ${headers} ${sources})
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if(_STREAMLINE_VERSION_MAJOR LESS 2)
  target_compile_definitions(${PROJECT_NAME} PUBLIC STREAMLINE_LEGACY)
endif()

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
#find_path(DIRECTX_INCLUDE_DIR d3dcompiler.h)
#find_library(D3D_COMPILER_LIB d3dcompiler.lib)
target_link_libraries("${PROJECT_NAME}" PUBLIC
        spdlog
        glm
        bddisasm
        shlwapi
        minhook
#        xbyak
        safetyhook
#        PolyHook_2
#        ViGEmClient
        imgui_lib
        kananlib
        openxr_loader
        delayimp
        DirectXTK
        DirectXTK12
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/openvr/lib/win64/openvr_api.lib"
)

# DirectXTK and DirectXTK12 share header names (SpriteBatch.h, GraphicsMemory.h): their headers are included as
# <directxtk-src/Inc/...> and <directxtk12-src/Inc/...> from the folder above each (FetchContent or local copy).
target_include_directories(${PROJECT_NAME} PUBLIC "${directxtk_SOURCE_DIR}/.." "${directxtk12_SOURCE_DIR}/..")

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
    $<INSTALL_INTERFACE:src>
    $<BUILD_INTERFACE:${SimpleIni_SOURCE_DIR}>
    "extern/nlohmann"
#   "${xbyak_SOURCE_DIR}/xbyak"
    "${minhook_SOURCE_DIR}/include"
    "${vigemclient_SOURCE_DIR}/include"
    "${minhook_SOURCE_DIR}/src/hde"
    "${openxr_SOURCE_DIR}/src"
    "${spdlog_SOURCE_DIR}/src"
    "${glm_SOURCE_DIR}"
#    "${CMAKE_CURRENT_SOURCE_DIR}/framework-imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imguizmo"
#    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imnodes"
    "${DIRECTX_NAMESPACED_INCLUDE_DIR}"
  PUBLIC
#    ${imgui_SOURCE_DIR}
    "extern/openvr/headers"
#    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui"
    $<BUILD_INTERFACE:${streamline_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${fidelityfx_SOURCE_DIR}/ffx-api/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

#add_compile_definitions(DEBUG_PROFILING_ENABLED)


if(POLYHOOK2_FUNCTION_HOOK)
    add_compile_definitions(POLYHOOK2_FUNCTION_HOOK)
    include("cmake/polyhook.cmake")
    target_link_libraries("${PROJECT_NAME}" PUBLIC PolyHook_2)
endif()

target_precompile_headers(
  ${PROJECT_NAME}
  PRIVATE
    include/PCH.h
)

target_compile_features(
  ${PROJECT_NAME}
  PUBLIC
    cxx_std_23
)

target_compile_options(
  ${PROJECT_NAME}
  PUBLIC
    $<$<CONFIG:Debug>:/EHa>
)

if(COMPILE_SHADERS)
  set(SHADER_HEADER_DIR ${CMAKE_BINARY_DIR}/shader-headers)
  file(MAKE_DIRECTORY ${SHADER_HEADER_DIR})

  # Build HLSL shaders
  add_custom_target(shaders)

  set_source_files_properties(debug_layer_vs.hlsl PROPERTIES ShaderType "vs_6_1" Entry "vs_main")
  set_source_files_properties(debug_layer_ps.hlsl PROPERTIES ShaderType "ps_6_1" Entry "ps_main")
  set_source_files_properties(motion_vector_correction_cs.hlsl PROPERTIES ShaderType "cs_6_1" Entry "CSMain")

  set(HLSL_SHADER_FILES
          debug_layer_ps.hlsl
          debug_layer_vs.hlsl
          motion_vector_correction_cs.hlsl
  )

  set(SHADER_DEFINES)
  if(INVERT_DEPTH_IN_SHADER)
      list(APPEND SHADER_DEFINES /DINVERTED_DEPTH)
  endif()
  if(SHADER_GOW_FIX)
      list(APPEND SHADER_DEFINES /DGOW_FIX)
  endif()

  foreach(FILE ${HLSL_SHADER_FILES})
    get_filename_component(FILE_WE ${FILE} NAME_WE)
  #  list(APPEND CSO_SHADER_FILES ${CMAKE_BINARY_DIR}/${FILE_WE}.cso)
    get_source_file_property(shadertype ${FILE} ShaderType)
    get_source_file_property(functionentry ${FILE} Entry)
    add_custom_command(TARGET shaders
                       COMMAND dxc.exe /nologo /E${functionentry} /T${shadertype} ${SHADER_DEFINES} $<IF:$<CONFIG:DEBUG>,/Od,/O3> /Zi /Fo ${CMAKE_BINARY_DIR}/${FILE_WE}.cso /Fh ${SHADER_HEADER_DIR}/${FILE_WE}.h /Fd ${CMAKE_BINARY_DIR}/${FILE_WE}.pdb ${FILE}
                       MAIN_DEPENDENCY ${FILE}
                       COMMENT "HLSL ${FILE}"
                       WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders/
                       VERBATIM)
  endforeach(FILE)

  add_dependencies(${PROJECT_NAME} shaders)
  target_include_directories(
          ${PROJECT_NAME}
          PRIVATE
          $<BUILD_INTERFACE:${SHADER_HEADER_DIR}>
  )
endif()


if(SIGNATURE_SCAN)
  add_compile_definitions(SIGNATURE_SCAN)
endif()

#add_compile_definitions(GOWR_DEMO_MODE)

#
#install(
#  TARGETS ${PROJECT_NAME}
#  DESTINATION ${CMAKE_INSTALL_LIBDIR}
#)

#if(CMAKE_BUILD_TYPE STREQUAL Debug)
#  set(BUILD_NAME Debug)
#else()
#  set(BUILD_NAME Release)
#endif()


#file(GLOB_RECURSE OUTPUT_DLLS ${CMAKE_CURRENT_SOURCE_DIR}/artifact/*.dll)
#file(GLOB_RECURSE OUTPUT_PDBS ${CMAKE_CURRENT_SOURCE_DIR}/artifact/*.pdb)

set_property(
  TARGET ${PROJECT_NAME}
  APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${OUTPUT_DLLS} ${OUTPUT_PDBS} ${CPM_SOURCE_CACHE} ${CMAKE_CURRENT_BINARY_DIR}/_deps
)
