set(PROCPARAM_SRCS
    procparams.cc
    params/advanced.cc
    params/locallab.cc
    params/raw.cc
    params/serdes.cc
)

set(RTENGINESOURCEFILES
    # We may override new/delete when memory profiling is enabled
    newdelete.cc

    alpha.cc
    ahd_demosaic_RT.cc
    amaze_demosaic_RT.cc
    aspectratios.cc
    badpixels.cc
    bayer_bilinear_demosaic.cc
    boxblur.cc
    canon_cr3_decoder.cc
    CA_correct_RT.cc
    calc_distort.cc
    camconst.cc
    capturesharpening.cc
    cfa_linedn_RT.cc
    ciecam02.cc
    cieimage.cc
    clutstore.cc
    color.cc
    colortemp.cc
    coord.cc
    cplx_wavelet_dec.cc
    curves.cc
    dcp.cc
    dcraw.cc
    dcrop.cc
    demosaic_algos.cc
    dfmanager.cc
    diagonalcurves.cc
    dirpyr_equalizer.cc
    dual_demosaic_RT.cc
    dynamicprofile.cc
    editorrequest.cc
    editorrequest_registry.cc
    eahd_demosaic.cc
    EdgePreservingDecomposition.cc
    fast_demo.cc
    ffmanager.cc
    filmnegativeproc.cc
    flatcurves.cc
    formats.cc
    FTblockDN.cc
    gamutwarning.cc
    gauss.cc
    green_equil_RT.cc
    guidedfilter.cc
    hilite_recon.cc
    histmatching.cc
    hphd_demosaic_RT.cc
    iccjpeg.cc
    iccstore.cc
    iimage.cc
    image16.cc
    image8.cc
    imagedata.cc
    imagedimensions.cc
    imagefloat.cc
    imageio.cc
    imagesource.cc
    improccoordinator.cc
    improcfun.cc
    impulse_denoise.cc
    init.cc
    ipdehaze.cc
    ipgrain.cc
    iplab2rgb.cc
    iplocallab.cc
    iplabregions.cc
    iplocalcontrast.cc
    ipresize.cc
    ipretinex.cc
    ipshadowshighlights.cc
    ipsharpen.cc
    ipsharpenedges.cc
    ipsoftlight.cc
    iptoneequalizer.cc
    iptransform.cc
    ipvibrance.cc
    ipwavelet.cc
    labimage.cc
    lcp.cc
    lensmetadata.cc
    lmmse_demosaic.cc
    loadinitial.cc
    metadata.cc
    munselllch.cc
    myfile.cc
    panasonic_decoders.cc
    pdaflinesfilter.cc
    perspectivecorrection.cc
    PF_correct_RT.cc
    pipettebuffer.cc
    pixelshift.cc
    previewimage.cc
    processingjob.cc
    profilestore.cc
    rawflatfield.cc
    rawimage.cc
    rawimagesource.cc
    rcd_demosaic.cc
    rt_algo.cc
    rtapp.cc
    rtlensfun.cc
    rtthumbnail.cc
    shmap.cc
    simpleprocess.cc
    spot.cc
    stdimagesource.cc
    stage/stage_buffer.cc
    stage/stage_buffer_state.cc
    stage/stage_builtins.cc
    stage/stage_cache.cc
    stage/stage_cache_integration.cc
    stage/stage_device.cc
    stage/stage_dirty_blocks.cc
    stage/stage_graph.cc
    stage/stage_indexer.cc
    stage/stage_lanczos.cc
    stage/stage_opencl_device.cc
    stage/stage_param_hash.cc
    stage/stage_param_map.cc
    stage/stage_parity.cc
    stage/stage_pipeline.cc
    stage/stage_profile.cc
    stage/stage_render_state.cc
    stage/stage_runtime.cc
    stage/stage_store.cc
    stage/stage_work.cc
    svgrender.cc
    tmo_fattal02.cc
    utils.cc
    vng4_demosaic_RT.cc
    xtrans_demosaic.cc
    ${PROCPARAM_SRCS}
)

if(RT_HAVE_OPENCL)
    list(APPEND RTENGINESOURCEFILES
        opencl/openclcache.cc
        opencl/openclengine.cc
        opencl/openclloader.cc
    )
else()
    list(APPEND RTENGINESOURCEFILES opencl/openclengine_stub.cc)
endif()

if(LENSFUN_HAS_LOAD_DIRECTORY)
    set_source_files_properties(
        rtlensfun.cc PROPERTIES
        COMPILE_DEFINITIONS RT_LENSFUN_HAS_LOAD_DIRECTORY
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12.0")
    # procparams.cc takes a long time to compile with optimizations starting
    # with GCC 12.1 due to PTA (see issue #6548)
    get_source_file_property(PROCPARAMS_COMPILE_OPTIONS procparams.cc COMPILE_OPTIONS)
    if(PROCPARAMS_COMPILE_OPTIONS STREQUAL "NOTFOUND")
        set(PROCPARAMS_COMPILE_OPTIONS "")
    else()
        set(PROCPARAMS_COMPILE_OPTIONS "${PROCPARAMS_COMPILE_OPTIONS};")
    endif()
    set(PROCPARAMS_COMPILE_OPTIONS "${PROCPARAMS_COMPILE_OPTIONS}-fno-tree-pta")

    set_source_files_properties(
        procparams.cc PROPERTIES
        COMPILE_OPTIONS ${PROCPARAMS_COMPILE_OPTIONS}
    )
endif()

add_subdirectory(stage)

add_library(rtengine STATIC "${RTENGINESOURCEFILES}")

add_dependencies(rtengine UpdateInfo)

target_link_libraries(rtengine PRIVATE rtstage_algorithms)
target_compile_definitions(rtengine PRIVATE
    RT_STAGE_BUILD_DIR="${CMAKE_BINARY_DIR}/stages"
)

# Standalone regression test for the opt-in preview base cache. It is not part
# of the normal build; use the target explicitly or tools/perf/test-preview-base-cache.sh.
add_executable(preview-base-cache-test EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/perf/preview-base-cache-test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(preview-base-cache-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(preview-base-cache-test PRIVATE rtengine)
rt_setup_target(preview-base-cache-test)

# Standalone regression test for the 100% FAST preview decision.
add_executable(fast-preview-test EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/perf/fast-preview-test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(fast-preview-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(fast-preview-test PRIVATE rtengine)
rt_setup_target(fast-preview-test)

# Focused legacy-vs-graph parity harness for the thumbnail stage graph.
add_executable(thumbnail-stage-test EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/perf/thumbnail-stage-test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(thumbnail-stage-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(thumbnail-stage-test PRIVATE rtengine)
rt_setup_target(thumbnail-stage-test)

# Focused legacy-vs-graph parity harness for the crop stage graph.
add_executable(crop-stage-test EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/perf/crop-stage-test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(crop-stage-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(crop-stage-test PRIVATE rtengine)
rt_setup_target(crop-stage-test)

# Deterministic preview raw-chain sweep through the real ImProcCoordinator.
add_executable(preview-work-order-sweep EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/perf/preview-work-order-sweep.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(preview-work-order-sweep BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(preview-work-order-sweep PRIVATE rtengine)
rt_setup_target(preview-work-order-sweep)

# Runtime stage ABI and built-in/module parity tests.
add_executable(stage-abi-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_abi_test.cc"
)
target_link_libraries(stage-abi-test PRIVATE rtengine)
target_include_directories(stage-abi-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
target_compile_definitions(stage-abi-test PRIVATE
    RT_STAGE_BUILD_DIR="${CMAKE_BINARY_DIR}/stages"
)
rt_setup_target(stage-abi-test)

add_executable(stage-cache-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_cache_test.cc"
)
target_link_libraries(stage-cache-test PRIVATE rtengine)
target_include_directories(stage-cache-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-cache-test)

# Node-output cache of the preview raw.base tier: key, decision, budget,
# eviction, counters, disabled path and repeated-pass parity.
add_executable(stage-node-cache-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_node_cache_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-node-cache-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-node-cache-test PRIVATE rtengine)
target_include_directories(stage-node-cache-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/stage"
)
rt_setup_target(stage-node-cache-test)

add_executable(stage-device-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_device_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-device-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-device-test PRIVATE rtengine)
target_include_directories(stage-device-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
target_compile_definitions(stage-device-test PRIVATE
    RT_STAGE_BUILD_DIR="${CMAKE_BINARY_DIR}/stages"
)
rt_setup_target(stage-device-test)

# Device residency: allocation, upload, readback, budget, eviction, event
# states, per-node selection and the first device node's parity.
add_executable(stage-residency-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_residency_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-residency-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-residency-test PRIVATE rtengine)
target_include_directories(stage-residency-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/stage"
)
rt_setup_target(stage-residency-test)

add_executable(stage-graph-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_graph_test.cc"
)
target_link_libraries(stage-graph-test PRIVATE rtengine)
target_include_directories(stage-graph-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-graph-test)

add_executable(stage-indexer-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_indexer_test.cc"
)
target_link_libraries(stage-indexer-test PRIVATE rtengine)
target_include_directories(stage-indexer-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-indexer-test)

# Stable parameter fingerprint used by per-stage cache keys.
add_executable(stage-param-hash-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_param_hash_test.cc"
)
target_link_libraries(stage-param-hash-test PRIVATE rtengine)
target_include_directories(stage-param-hash-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-param-hash-test)

# Explicit render state that is not represented in ProcParams.
add_executable(stage-render-state-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_render_state_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-render-state-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-render-state-test PRIVATE rtengine)
target_include_directories(stage-render-state-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-render-state-test)

# Export descriptor to ProcParams member mapping, proved by mutation.
add_executable(stage-param-map-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_param_map_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-param-map-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-param-map-test PRIVATE rtengine)
target_include_directories(stage-param-map-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-param-map-test)

# Dirty-block derivation from parameter fingerprints and render state.
add_executable(stage-dirty-blocks-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_dirty_blocks_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-dirty-blocks-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-dirty-blocks-test PRIVATE rtengine)
target_include_directories(stage-dirty-blocks-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stage"
)
rt_setup_target(stage-dirty-blocks-test)

# Exact raw/core work orders derived from fingerprints and request facts.
add_executable(stage-work-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_work_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-work-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-work-test PRIVATE rtengine)
target_include_directories(stage-work-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stage"
)
rt_setup_target(stage-work-test)

add_executable(stage-parity-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_parity_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(stage-parity-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(stage-parity-test PRIVATE rtengine)
target_include_directories(stage-parity-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
target_compile_definitions(stage-parity-test PRIVATE
    RT_STAGE_BUILD_DIR="${CMAKE_BINARY_DIR}/stages"
)
rt_setup_target(stage-parity-test)

# The pipeline specification is data only, so the test compiles the
# specification unit directly instead of linking the whole engine.
add_executable(stage-pipeline-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_pipeline_test.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/stage_pipeline.cc"
)
target_include_directories(stage-pipeline-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-pipeline-test)

add_executable(stage-store-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/stage_store_test.cc"
)
target_link_libraries(stage-store-test PRIVATE rtengine)
target_include_directories(stage-store-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stage")
rt_setup_target(stage-store-test)

# Typed editor request and its single legacy-mask adapter.
add_executable(editorrequest-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/editorrequest_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(editorrequest-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(editorrequest-test PRIVATE rtengine)
target_compile_definitions(editorrequest-test PRIVATE
    RT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
rt_setup_target(editorrequest-test)

# Decision-table replay for the typed viewport fast-path admission and LUT
# selection, against the retired mask-based gate kept as a local oracle.
add_executable(viewport-admission-test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/stage/tests/viewport_admission_test.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/editcallbacks.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/multilangmgr.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/options.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/paramsedited.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/pathutils.cc"
    "${PROJECT_SOURCE_DIR}/rtgui/threadutils.cc"
)
target_include_directories(viewport-admission-test BEFORE PRIVATE
    "${CMAKE_BINARY_DIR}/rtgui"
)
target_link_libraries(viewport-admission-test PRIVATE rtengine)
rt_setup_target(viewport-admission-test)

target_include_directories(rtengine
    PUBLIC
        # Other libraries should use #include "rtengine/header.h"
        ${PROJECT_SOURCE_DIR}
    PRIVATE
        # Subdirectories that are not libraries themselves should be included
        # like #include "your_subdirectory/header.h"
        ${CMAKE_CURRENT_SOURCE_DIR}
)
# UpdateInfo creates "rtgui/version.h"
target_include_directories(rtengine BEFORE PUBLIC ${CMAKE_BINARY_DIR})

if(EXTRA_INCDIR)
    target_include_directories(rtengine PUBLIC "${EXTRA_INCDIR}")
endif()

rt_setup_target(rtengine)
target_compile_options(rtengine PRIVATE ${RTENGINE_CXX_FLAGS})

if(RT_HAVE_OPENCL)
    target_compile_definitions(rtengine PRIVATE RT_HAVE_OPENCL CL_TARGET_OPENCL_VERSION=120)
    target_include_directories(rtengine PRIVATE ${OPENCL_INCLUDE_DIR})
endif()

if(WITH_BENCHMARK)
    target_compile_definitions(rtengine PUBLIC "BENCHMARK")
endif()

target_link_libraries(rtengine
    PUBLIC
        # When linking against RT's version of libraw, there will be linking
        # issues for jpeg and zlib if libraw is not linked before them.
        External::LibRaw
        External::cjson
        External::jpeg_ijg
        External::klt
        fmt::fmt
        PkgConfig::CAIROMM
        PkgConfig::EXIV2
        PkgConfig::EXPAT
        PkgConfig::FFTW3F
        PkgConfig::GLIBMM
        PkgConfig::GTKMM
        PkgConfig::IPTCDATA
        PkgConfig::LCMS
        PkgConfig::LENSFUN
        PNG::PNG
        TIFF::TIFF
        ZLIB::ZLIB
        ${EXTRA_LIB}
)

# Atomic is required in some builds: #6821
if(ATOMIC_FOUND)
    target_link_libraries(rtengine PUBLIC ${ATOMIC_LIBRARIES})
endif()

if(JXL_FOUND)
    target_link_libraries(rtengine PUBLIC PkgConfig::JXL)
    if (JXLTHREADS_FOUND)
        target_link_libraries(rtengine PUBLIC PkgConfig::JXLTHREADS)
    endif()
endif()

if(OpenMP_CXX_FOUND)
    target_link_libraries(rtengine PUBLIC OpenMP::OpenMP_CXX)
endif()

if("${SVG_BACKEND}" STREQUAL "librsvg")
    target_link_libraries(rtengine PUBLIC PkgConfig::RSVG)
    target_compile_definitions(rtengine PUBLIC "SVG_BACKEND_RSVG")
elseif("${SVG_BACKEND}" STREQUAL "lunasvg")
    if(WITH_SYSTEM_LUNASVG)
        target_link_libraries(rtengine PUBLIC PkgConfig::LUNASVG)
    else()
        target_link_libraries(rtengine PUBLIC lunasvg::lunasvg)
    endif()
    target_compile_definitions(rtengine PUBLIC "SVG_BACKEND_LUNASVG")
else()
    message(FATAL_ERROR "Unknown SVG rendering backend")
endif()

if(TCMALLOC_FOUND)
    target_compile_definitions(rtengine PUBLIC "RT_USING_TCMALLOC")
endif()

if(WITH_TRACY_PROFILER)
    target_link_libraries(rtengine PUBLIC Tracy::TracyClient)
    target_compile_definitions(rtengine PUBLIC "RT_TRACING_ENABLE")

    if(WITH_TRACY_MEMORY_PROFILING)
        if(TCMALLOC_FOUND AND NOT WITH_TRACY_ON_DEMAND)
            message(SEND_ERROR "-DWITH_TRACY_ON_DEMAND=ON is required for memory profiling with tcmalloc")
        endif()
        target_compile_definitions(rtengine PUBLIC "WITH_TRACY_MEMORY_PROFILING")
    endif()
endif()

# It may be nice to store library version too
if(BUILD_SHARED_LIBS)
    install(TARGETS rtengine DESTINATION "${LIBDIR}")
endif()

set(CAMCONSTSFILE "camconst.json")
install(FILES ${CAMCONSTSFILE} DESTINATION "${DATADIR}" PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
