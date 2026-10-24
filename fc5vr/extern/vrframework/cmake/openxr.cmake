include(FetchContent)

# Safetyhook
FetchContent_Declare(
        openxr
        GIT_REPOSITORY "https://github.com/KhronosGroup/OpenXR-SDK.git"
        GIT_TAG "458984d7f59d1ae6dc1b597d94b02e4f7132eaba"
)
message("Fetching openxr")
FetchContent_MakeAvailable(openxr)

