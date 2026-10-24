include(FetchContent)

FetchContent_Declare(
        ViGEmClient
        GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
        GIT_TAG        b66d02d57e32cc8595369c53418b843e958649b4
        GIT_PROGRESS TRUE
)
message("fetching viGemBusClient")
FetchContent_MakeAvailable(ViGEmClient )
