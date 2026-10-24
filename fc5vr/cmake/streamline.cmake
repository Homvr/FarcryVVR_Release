include(FetchContent)

# Safetyhook
FetchContent_Declare(
        streamline
        GIT_REPOSITORY "https://github.com/NVIDIAGameWorks/Streamline.git"
        GIT_TAG "v2.4.11"
)
message("Fetching streamline")
FetchContent_MakeAvailable(streamline)
set_property(TARGET streamline PROPERTY CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(TARGET streamline PROPERTY STREAMLINE_IMPORT_AS_INTERFACE ON)

