include(FetchContent)

# FidelityFX SDK - headers only (for FSR 2/3 struct layouts)
FetchContent_Declare(
        fidelityfx
        GIT_REPOSITORY "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git"
        GIT_TAG "v1.1.4"
)
message("Fetching FidelityFX SDK (headers only)")
FetchContent_Populate(fidelityfx)

