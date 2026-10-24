include(FetchContent)

FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        bf71a834948186f4097caa076cd2663c69a10e1e
        GIT_PROGRESS TRUE
)
message("fetching glm")
FetchContent_MakeAvailable(glm)

#add_compile_definitions(GLM_FORCE_LEFT_HANDED)
