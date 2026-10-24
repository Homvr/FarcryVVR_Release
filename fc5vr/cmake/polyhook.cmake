include(FetchContent)

# Safetyhook
FetchContent_Declare(
        PolyHook_2
        GIT_REPOSITORY "https://github.com/stevemk14ebr/PolyHook_2_0.git"
        GIT_TAG "origin/master"
)
message("Fetching PolyHook_2_0")
FetchContent_MakeAvailable(PolyHook_2)
#set_property(TARGET PolyHook_2 PROPERTY BUILD_STATIC_RUNTIME  ON)
target_compile_options(
    PolyHook_2
    PRIVATE
       $<$<CONFIG:Debug>:/MDd>
       $<$<NOT:$<CONFIG:Debug>>:/MD>
)
target_compile_options(
    asmjit
    PRIVATE
       $<$<CONFIG:Debug>:/MDd>
       $<$<NOT:$<CONFIG:Debug>>:/MD>
)
target_compile_options(
    asmtk
    PRIVATE
       $<$<CONFIG:Debug>:/MDd>
       $<$<NOT:$<CONFIG:Debug>>:/MD>
)
#target_compile_definitions(PolyHook_2 PRIVATE _ITERATOR_DEBUG_LEVEL=0)
