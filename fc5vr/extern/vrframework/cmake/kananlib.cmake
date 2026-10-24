include(FetchContent)
message("Fetching bddisasm (v1.37.0)...")
FetchContent_Declare(bddisasm
        GIT_REPOSITORY
        "https://github.com/bitdefender/bddisasm"
        GIT_TAG
        v1.37.0
)
FetchContent_MakeAvailable(bddisasm)

# kananlib
FetchContent_Declare(
        kananlib
        GIT_REPOSITORY "https://github.com/cursey/kananlib.git"
        GIT_TAG 05fe2456b58fa423531d036d9343ae46866d7ffd
#        PATCH_COMMAND git apply --ignore-whitespace "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kananlib-clang.patch"
#        UPDATE_DISCONNECTED 1
)


message("Fetching kananlib")
FetchContent_MakeAvailable(kananlib)
target_compile_definitions(kananlib
        PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:_ThrowInfo=ThrowInfo>
        $<$<CXX_COMPILER_ID:Clang>:NTSTATUS=LONG>
)
# target_compile_options(kananlib
#         PRIVATE
#         $<$<CXX_COMPILER_ID:Clang>:-mavx -mavx2>
# )
#target_compile_options(kananlib PUBLIC
#        "/EHsc"
#)
#set_property(TARGET kananlib PROPERTY CXX_STANDARD 23)
#set_property(TARGET kananlib PROPERTY KANANLIB_FETCH_BDDISASM ON)