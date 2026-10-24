include(FetchContent)

# directxtk
#FetchContent_Declare(
#        DirectXTK
#        GIT_REPOSITORY "https://github.com/microsoft/DirectXTK.git"
#        GIT_TAG "f5026eb34e7053b1aff325d38db107703f394974"
#)
#message("Fetching directxtk")
#FetchContent_MakeAvailable(DirectXTK)

# directxtk
FetchContent_Declare(
        DirectXTK12
        GIT_REPOSITORY "https://github.com/microsoft/DirectXTK12.git"
        GIT_TAG "528801aa6dd8d628c2f756c41a76d300f47de478"
)
message("Fetching directxtk12")
FetchContent_MakeAvailable(DirectXTK12)
if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.35)
#    target_compile_options(DirectXTK PRIVATE /Zc:templateScope-)
    target_compile_options(DirectXTK12 PRIVATE /Zc:templateScope-)
endif()