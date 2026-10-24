include(FetchContent)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        069a2e8fc947f63855d770fdc3c3eb427f19988f
        GIT_PROGRESS TRUE
)
message("fetching spdlog")
FetchContent_MakeAvailable(spdlog)
