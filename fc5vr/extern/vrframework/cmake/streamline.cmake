include(FetchContent)

string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" STREAMLINE_VERSION_MATCH "${STREAMLINE_VERSION}")
if (STREAMLINE_VERSION_MATCH)
    set(_STREAMLINE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(_STREAMLINE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(_STREAMLINE_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
    set(_STREAMLINE_VERSION_MAJOR 2)
    set(_STREAMLINE_VERSION_MINOR 4)
    set(_STREAMLINE_VERSION_PATCH 2)
endif()

if (_STREAMLINE_VERSION_MAJOR LESS 2)
    set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS STREAMLINE_LEGACY)
endif()

# Streamline - headers only
FetchContent_Declare(
        streamline
        GIT_REPOSITORY "https://github.com/NVIDIAGameWorks/Streamline.git"
        GIT_TAG "${STREAMLINE_VERSION}"
)
message("Fetching streamline ${STREAMLINE_VERSION}")
FetchContent_Populate(streamline)

