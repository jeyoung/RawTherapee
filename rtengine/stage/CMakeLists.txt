# Runtime-loadable stage algorithms and modules.
#
# The algorithm library is position independent because it is linked into both
# the host executable and the loadable modules. It must not depend on rtengine
# globals; modules are deliberately stateless.

add_library(rtstage_algorithms STATIC
    algorithms/lanczos.cc
)
set_target_properties(rtstage_algorithms PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(rtstage_algorithms PRIVATE
    "${PROJECT_SOURCE_DIR}/rtengine"
)
target_link_libraries(rtstage_algorithms PRIVATE OpenMP::OpenMP_CXX)
rt_setup_target(rtstage_algorithms)

if(RT_STAGE_MODULES)
    add_library(stage_lanczos MODULE
        adapters/stage_lanczos_module.cc
        stage_lanczos.cc
    )
    set_target_properties(stage_lanczos PROPERTIES
        PREFIX ""
        OUTPUT_NAME "lanczos3"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stages"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(stage_lanczos PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/rtengine"
    )
    target_link_libraries(stage_lanczos PRIVATE
        rtstage_algorithms
        OpenMP::OpenMP_CXX
    )
    rt_setup_target(stage_lanczos)

    add_custom_target(stage_modules ALL DEPENDS stage_lanczos)
else()
    add_custom_target(stage_modules)
endif()
