include(FetchContent)

set(GLM_FORCE_LEFT_HANDED ON CACHE BOOL "Force GLM to use left-handed coordinate system" FORCE)
set(GLM_FORCE_DEPTH_ZERO_TO_ONE ON CACHE BOOL "Force GLM to use directX spec" FORCE)
set(GLM_FORCE_WIN32 ON CACHE BOOL "Force GLM to use left-handed coordinate system" FORCE)
set(GLM_FORCE_SSE2 ON CACHE BOOL "Force GLM to use left-handed coordinate system" FORCE)
set(GLM_FORCE_INLINE ON CACHE BOOL "Force GLM to use left-handed coordinate system" FORCE)
#set(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES ON CACHE BOOL "Force GLM to use left-handed coordinate system" FORCE)

FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        bf71a834948186f4097caa076cd2663c69a10e1e
        GIT_PROGRESS TRUE
)
message("fetching glm")
FetchContent_MakeAvailable(glm)

target_compile_definitions(glm INTERFACE GLM_FORCE_LEFT_HANDED)
target_compile_definitions(glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_compile_definitions(glm INTERFACE GLM_FORCE_WIN32)
target_compile_definitions(glm INTERFACE GLM_FORCE_SSE2)
target_compile_definitions(glm INTERFACE GLM_FORCE_INLINE)
#target_compile_definitions(glm INTERFACE GLM_FORCE_DEFAULT_ALIGNED_GENTYPES)