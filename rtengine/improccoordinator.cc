/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <glib.h>
#include <glibmm/thread.h>

#include "improccoordinator.h"
#include "opencl/openclengine.h"

#include "array2D.h"
#include "cieimage.h"
#include "color.h"
#include "colortemp.h"
#include "curves.h"
#include "dcp.h"
#include "guidedfilter.h"
#include "iccstore.h"
#include "image8.h"
#include "imagefloat.h"
#include "improcfun.h"
#include "metadata.h"
#include "labimage.h"
#include "lcp.h"
#include "loadprofile.h"
#include "procparams.h"
#include "previewbasecache.h"
#include "previewquality.h"
#include "profiling.h"
#include "stage/stage_graph.h"
#include "stage/stage_profile.h"
#include "tweakoperator.h"
#include "refreshmap.h"
#include "utils.h"
#include "rt_algo.h"
#include "viewportdefer.h"
#include "rtgui/labgrid.h"

#include "rtgui/options.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace
{

constexpr int VECTORSCOPE_SIZE = 128;

const char* baseCacheTodoReason(int todo)
{
    if (todo & M_PREPROC) {
        return "preprocess";
    }
    if (todo & M_RAW) {
        return "raw";
    }
    if (todo & M_WB) {
        return "white-balance";
    }
    if (todo & M_HIGHQUAL) {
        return "high-detail-request";
    }
    // M_TRANSFORM is part of ALLNORAW and usually only changes the post-base
    // transform. The base transform bit mask is part of the cache key instead.
    return nullptr;
}

}

namespace
{

struct PreviewTimer {
    bool enabled = false;
    bool cacheHit = false;
    bool viewportFast = false;
    bool viewportDeferred = false;
    const char* viewportFallback = nullptr;
    long long viewportCropMs = -1;
    int viewportDeferredTodo = 0;
    int viewportDeferredHits = 0;
    int todo = 0;
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    PreviewTimer(int changeMask, bool isEnabled)
        : enabled(isEnabled), todo(changeMask)
    {
    }

    ~PreviewTimer()
    {
        if (!enabled) {
            return;
        }

        const auto end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        std::fprintf(stderr, "preview update: %lld ms todo=0x%x cache=%d",
                     static_cast<long long>(ms), todo, cacheHit ? 1 : 0);
        if (viewportFast) {
            std::fprintf(stderr,
                         " viewport=fast crop_ms=%lld deferred_todo=0x%x"
                         " deferred_hits=%d\n",
                         viewportCropMs, viewportDeferredTodo,
                         viewportDeferredHits);
        } else if (viewportDeferred) {
            std::fprintf(stderr, " viewport=deferred-analysis\n");
        } else if (viewportFallback) {
            std::fprintf(stderr, " viewport=fallback reason=%s\n",
                         viewportFallback);
        } else {
            std::fputc('\n', stderr);
        }
    }
};

} // namespace

namespace rtengine
{

ImProcCoordinator::ImProcCoordinator() :
    orig_prev(nullptr),
    oprevi(nullptr),
    spotprev(nullptr),
    oprevl(nullptr),
    nprevl(nullptr),
    fattal_11_dcrop_cache(nullptr),
    previmg(nullptr),
    workimg(nullptr),
    ncie(nullptr),
    imgsrc(nullptr),
    lastAwbEqual(0.),
    lastAwbTempBias(0.0),
    lastAwbauto(""),
    monitorIntent(RI_RELATIVE),
    softProof(false),
    gamutCheck(false),
    sharpMask(false),
    sharpMaskChanged(false),
    scale(10),
    highDetailPreprocessComputed(false),
    highDetailRawComputed(false),
    highDetailStateKnown(false),
    lastHighDetailNeeded(false),
    fastPreviewEnabled(false),
    mainPreviewFullResolution(false),
    paramsReady(false),
    lastPassPrevdemoBase(PrevdemoBase::UNKNOWN),
    viewportStateValid(false),
    viewportHighDetail(false),
    forceFullPreview(false),
    viewportDeferredRequest(0),
    deferredCropRendered(false),
    viewportZeroLhist(32768),
    previewBaseCache(new PreviewBaseCache()),
    allocated(false),
    bwAutoR(-9000.f),
    bwAutoG(-9000.f),
    bwAutoB(-9000.f),
    CAMMean(NAN),
    hltonecurve(65536),
    shtonecurve(65536),
    tonecurve(65536, 0),  //,1);
    lumacurve(32770, 0),  // lumacurve[32768] and lumacurve[32769] will be set to 32768 and 32769 later to allow linear interpolation
    chroma_acurve(65536, 0),
    chroma_bcurve(65536, 0),
    satcurve(65536, 0),
    lhskcurve(65536, 0),
    clcurve(65536, 0),
    conversionBuffer(1, 1),
    wavclCurve(65536, 0),
    clToningcurve(65536, 0),
    cl2Toningcurve(65536, 0),
    Noisecurve(65536, 0),
    NoiseCCcurve(65536, 0),
    vhist16(65536), vhist16bw(65536),
    lhist16CAM(65536),
    lhist16CCAM(65536),
    lhist16RETI(),
    lhist16LClad(65536),
    histRed(256), histRedRaw(256),
    histGreen(256), histGreenRaw(256),
    histBlue(256), histBlueRaw(256),
    histLuma(256),
    histToneCurve(256),
    histToneCurveBW(256),
    histLCurve(256),
    histCCurve(256),
    histLLCurve(256),

    histLCAM(256),
    histCCAM(256),
    histClad(256),
    bcabhist(256),
    histChroma(256),

    histLRETI(256),

    hist_lrgb_dirty(false),
    hist_raw_dirty(false),

    vectorscopeScale(0),
    vectorscope_hc_dirty(false),
    vectorscope_hs_dirty(false),
    vectorscope_hc(VECTORSCOPE_SIZE, VECTORSCOPE_SIZE),
    vectorscope_hs(VECTORSCOPE_SIZE, VECTORSCOPE_SIZE),
    waveformScale(0),
    waveform_dirty(false),
    waveformRed(0, 0),
    waveformGreen(0, 0),
    waveformBlue(0, 0),
    waveformLuma(0, 0),

    CAMBrightCurveJ(), CAMBrightCurveQ(),

    rCurve(),
    gCurve(),
    bCurve(),
    ctColorCurve(),
    rcurvehist(256), rcurvehistCropped(256), rbeforehist(256),
    gcurvehist(256), gcurvehistCropped(256), gbeforehist(256),
    bcurvehist(256), bcurvehistCropped(256), bbeforehist(256),
    fw(0), fh(0), tr(0),
    fullw(1), fullh(1),
    pW(-1), pH(-1),
    plistener(nullptr),
    imageListener(nullptr),
    aeListener(nullptr),
    acListener(nullptr),
    ablListener(nullptr),
    abwListener(nullptr),
    awbListener(nullptr),
    flatFieldAutoClipListener(nullptr),
    bayerAutoContrastListener(nullptr),
    xtransAutoContrastListener(nullptr),
    pdSharpenAutoContrastListener(nullptr),
    pdSharpenAutoRadiusListener(nullptr),
    frameCountListener(nullptr),
    imageTypeListener(nullptr),
    filmNegListener(nullptr),
    actListener(nullptr),
    primListener(nullptr),
    adnListener(nullptr),
    awavListener(nullptr),
    dehaListener(nullptr),
    acmaxListener(nullptr),
    hListener(nullptr),
    resultValid(false),
    params(new procparams::ProcParams),
    tweakOperator(nullptr),
    lastOutputProfile("BADFOOD"),
    lastOutputIntent(RI__COUNT),
    lastOutputBPC(false),
    thread(nullptr),
    changeSinceLast(0),
    updaterRunning(false),
    nextParams(new procparams::ProcParams),
    destroying(false),
    pendingDeferredMask(0),
    pendingDeferredHits(0),
    pendingDeferredFirstUs(0),
    pendingDeferredDeadlineUs(0),
    deferredWatcherStarted(false),
    deferredWatcherPaused(false),
    deferredWatcherStop(false),
    deferredWatcher(nullptr),
    deferredCropReusable(false),
    utili(false),
    autili(false),
    butili(false),
    ccutili(false),
    cclutili(false),
    clcutili(false),
    opautili(false),
    wavcontlutili(false),
    colourToningSatLimit(0.f),
    colourToningSatLimitOpacity(0.f),
    highQualityComputed(false),
    customTransformIn(nullptr),
    customTransformOut(nullptr),
    ipf(params.get(), true),

    // Locallab
    locallListener(nullptr),
    lllocalcurve(65536, LUT_CLIP_OFF),
    cllocalcurve(65536, LUT_CLIP_OFF),
    lclocalcurve(65536, LUT_CLIP_OFF),
    cclocalcurve(65536, LUT_CLIP_OFF),
    rgblocalcurve(65536, LUT_CLIP_OFF),
    exlocalcurve(65536, LUT_CLIP_OFF),
    hltonecurveloc(65536, LUT_CLIP_OFF), //32768
    shtonecurveloc(65536, LUT_CLIP_OFF),
    tonecurveloc(65536, LUT_CLIP_OFF),
    lightCurveloc(32770, LUT_CLIP_OFF),
    lmasklocalcurve(65536, LUT_CLIP_OFF),
    lmaskexplocalcurve(65536, LUT_CLIP_OFF),
    lmaskSHlocalcurve(65536, LUT_CLIP_OFF),
    ghslocalcurve(65536, LUT_CLIP_OFF),
    lmaskviblocalcurve(65536, LUT_CLIP_OFF),
    lmasktmlocalcurve(65536, LUT_CLIP_OFF),
    lmaskretilocalcurve(65536, LUT_CLIP_OFF),
    lmaskcblocalcurve(65536, LUT_CLIP_OFF),
    lmaskbllocalcurve(65536, LUT_CLIP_OFF),
    lmasklclocalcurve(65536, LUT_CLIP_OFF),
    lmaskloglocalcurve(65536, LUT_CLIP_OFF),
    lmasklocal_curve(65536, LUT_CLIP_OFF),
    lmaskcielocalcurve(65536, LUT_CLIP_OFF),
    cielocalcurve(65536, LUT_CLIP_OFF),
    cielocalcurve2(65536, LUT_CLIP_OFF),
    jzlocalcurve(65536, LUT_CLIP_OFF),
    czlocalcurve(65536, LUT_CLIP_OFF),
    czjzlocalcurve(65536, LUT_CLIP_OFF),
    redlocalcurve(65536, LUT_CLIP_OFF),
    greenlocalcurve(65536, LUT_CLIP_OFF),
    bluelocalcurve(65536, LUT_CLIP_OFF),

    lastspotdup(false),
    previewDeltaE(false),
    locallColorMask(0),
    locallColorMaskinv(0),
    locallExpMask(0),
    locallExpMaskinv(0),
    locallSHMask(0),
    locallSHMaskinv(0),
    locallvibMask(0),
    localllcMask(0),
    locallcbMask(0),
    locallretiMask(0),
    locallsoftMask(0),
    localltmMask(0),
    locallblMask(0),
    locallsharMask(0),
    localllogMask(0),
    locall_Mask(0),
    locallcieMask(0),
    retistrsav(nullptr)
{
    viewportZeroLhist.clear();
    if (settings != nullptr && !settings->cacheDir.empty()) {
        previewBaseCache->setPersistentRoot(
            (settings->cacheDir + "/stages").raw());
    }
}

ImProcCoordinator::~ImProcCoordinator()
{

    destroying = true;
    deferredWatcherStop.store(true, std::memory_order_release);

    Glib::Thread* watcher = nullptr;
    {
        MyMutex::MyLock lock(paramsUpdateMutex);
        watcher = deferredWatcher;
        deferredWatcher = nullptr;
        deferredWatcherStarted = false;
        deferredWatcherPaused = true;
        pendingDeferredMask = 0;
        pendingDeferredHits = 0;
        pendingDeferredFirstUs = 0;
        pendingDeferredDeadlineUs = 0;
    }

    if (watcher) {
        watcher->join();
    }

    updaterThreadStart.lock();

    if (updaterRunning && thread) {
        thread->join();
    }

    mProcessing.lock();
    mProcessing.unlock();
    freeAll();
    previewBaseCache->printSummary();

    if (fattal_11_dcrop_cache) {
        delete fattal_11_dcrop_cache;
        fattal_11_dcrop_cache = nullptr;
    }

    std::vector<Crop*> toDel = crops;

    for (size_t i = 0; i < toDel.size(); i++) {
        delete toDel[i];
    }

    imgsrc->decreaseRef();

    if (customTransformIn) {
        cmsDeleteTransform(customTransformIn);
        customTransformIn = nullptr;
    }

    if (customTransformOut) {
        cmsDeleteTransform(customTransformOut);
        customTransformOut = nullptr;
    }

    updaterThreadStart.unlock();
}

void ImProcCoordinator::assign(ImageSource* imgsrc)
{
    this->imgsrc = imgsrc;
    previewBaseCache->clear("image-switch");
    initialRoughPreview = false;
    invalidateViewportState();

    // A new image source has no completed high-detail base state. Buffer
    // reallocations (freeAll) do not change this because the source planes are
    // still valid.
    highDetailPreprocessComputed = false;
    highDetailRawComputed = false;

    mainPreviewFullResolution.store(false, std::memory_order_relaxed);
    paramsReady.store(false, std::memory_order_relaxed);
    lastPassPrevdemoBase.store(PrevdemoBase::UNKNOWN,
                               std::memory_order_relaxed);
}

void ImProcCoordinator::getParams(procparams::ProcParams* dst, bool tweaked)
{
    if (!tweaked && paramsBackup.operator bool()) {
        *dst = *paramsBackup;
    } else {
        *dst = *params;
    }
}

void ImProcCoordinator::backupParams()
{
    if (!params) {
        return;
    }

    if (!paramsBackup) {
        paramsBackup.reset(new ProcParams());
    }

    *paramsBackup = *params;
}

void ImProcCoordinator::restoreParams()
{
    if (!paramsBackup || !params) {
        return;
    }

    *params = *paramsBackup;
}

namespace
{

bool sameHighlightRecovery(const procparams::ToneCurveParams& a,
                           const procparams::ToneCurveParams& b)
{
    return a.hrenabled == b.hrenabled
        && a.method == b.method
        && a.hlbl == b.hlbl
        && a.hlth == b.hlth
        && a.clampOOG == b.clampOOG;
}

bool sameBaseInputs(const procparams::ProcParams& a,
                    const procparams::ProcParams& b)
{
    return a.raw == b.raw
        && a.wb == b.wb
        && a.icm == b.icm
        && a.coarse == b.coarse
        && a.commonTrans == b.commonTrans
        && a.rotate == b.rotate
        && a.distortion == b.distortion
        && a.lensProf == b.lensProf
        && a.perspective == b.perspective
        && a.gradient == b.gradient
        && a.pcvignette == b.pcvignette
        && a.cacorrection == b.cacorrection
        && a.vignetting == b.vignetting
        && a.spot == b.spot
        && a.filmNegative == b.filmNegative
        && a.dehaze == b.dehaze
        && a.fattal == b.fattal
        && a.retinex == b.retinex
        && a.pdsharpening == b.pdsharpening
        && a.dirpyrDenoise == b.dirpyrDenoise
        && a.cg == b.cg
        && a.locallab == b.locallab
        && a.toneCurve.autoexp == b.toneCurve.autoexp
        && a.toneCurve.histmatching == b.toneCurve.histmatching
        && sameHighlightRecovery(a.toneCurve, b.toneCurve);
}

bool viewportFastEnabled()
{
    // Enabled by default. RT_VIEWPORT_FAST=0 is the kill switch for
    // comparison and troubleshooting; other non-empty values keep it on.
    static const bool enabled = []() {
        const char* value = g_getenv("RT_VIEWPORT_FAST");
        return value == nullptr || value[0] == '\0'
            || g_strcmp0(value, "0") != 0;
    }();
    return enabled;
}

int viewportDeferredEnvMs(const char* name, int defaultValue)
{
    const char* value = g_getenv(name);

    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }

    char* end = nullptr;
    const long long parsed = g_ascii_strtoll(value, &end, 10);

    if (end == value) {
        return defaultValue;
    }

    return static_cast<int>(std::clamp<long long>(
        parsed, VIEWPORT_DEFERRED_SETTLE_MS_MIN,
        VIEWPORT_DEFERRED_SETTLE_MS_MAX));
}

int viewportDeferredSettleMs()
{
    // Unit: ms. Range: VIEWPORT_DEFERRED_SETTLE_MS_MIN..MAX. 0 queues the
    // deferred analysis immediately, reproducing the pre-deferral behaviour.
    static const int value = viewportDeferredEnvMs(
        "RT_VIEWPORT_DEFER_MS", VIEWPORT_DEFERRED_SETTLE_MS_DEFAULT);
    return value;
}

int viewportDeferredMaxWaitMs()
{
    // Unit: ms, measured from the oldest deferral hit. Range:
    // VIEWPORT_DEFERRED_SETTLE_MS_MIN..MAX.
    static const int value = viewportDeferredEnvMs(
        "RT_VIEWPORT_DEFER_MAX_MS", VIEWPORT_DEFERRED_MAX_WAIT_MS_DEFAULT);
    return value;
}

constexpr int VIEWPORT_FAST_ALLOWED_TODO =
    M_AUTOEXP | M_RGBCURVE | M_LUMACURVE | M_LUMINANCE | M_COLOR
    | M_CROP | M_MINUPDATE | M_HDR | M_SPOT;

constexpr int VIEWPORT_FAST_REJECTED_TODO =
    M_PREPROC | M_RAW | M_WB | M_INIT | M_LINDENOISE | M_MONITOR
    | M_TRANSFORM | M_CSHARP | M_RETINEX | M_HIGHQUAL | M_BLURMAP;

// Bits that are safe for Crop::update once the parent LUTs have been rebuilt.
// Base/init and over-approximated HDR/spot bits are deliberately dropped.
constexpr int VIEWPORT_CROP_TODO =
    M_CROP | M_MINUPDATE | M_AUTOEXP | M_TRANSFORM | M_RGBCURVE
    | M_LUMACURVE | M_LUMINANCE | M_COLOR;

} // namespace

bool ImProcCoordinator::canUseViewportFastPath(int todo,
                                               bool highDetailNeeded,
                                               const char*& reason) const
{
    reason = nullptr;

    if (!viewportFastEnabled()) {
        reason = "switch-off";
        return false;
    }
    if (!viewportStateValid.load(std::memory_order_relaxed)) {
        reason = "no-complete-full-state";
        return false;
    }
    if (highDetailNeeded
            != viewportHighDetail.load(std::memory_order_relaxed)) {
        reason = "high-detail-transition";
        return false;
    }
    if (highDetailNeeded
            && (!highDetailPreprocessComputed || !highDetailRawComputed)) {
        reason = "high-detail-pending";
        return false;
    }
    if (todo & VIEWPORT_FAST_REJECTED_TODO) {
        reason = "base-request";
        return false;
    }
    if (todo & ~VIEWPORT_FAST_ALLOWED_TODO) {
        reason = "unsupported-request";
        return false;
    }

    if (params->toneCurve.autoexp
            || viewportFullParams.toneCurve.autoexp) {
        reason = "auto-exposure";
        return false;
    }
    if (params->toneCurve.histmatching
            || viewportFullParams.toneCurve.histmatching
            || params->toneCurve.fromHistMatching
            || viewportFullParams.toneCurve.fromHistMatching) {
        reason = "histogram-matching";
        return false;
    }
    if ((params->blackwhite.enabled && params->blackwhite.autoc)
            || (viewportFullParams.blackwhite.enabled
                && viewportFullParams.blackwhite.autoc)) {
        reason = "auto-black-white";
        return false;
    }
    if ((params->colorToning.enabled && params->colorToning.autosat)
            || (viewportFullParams.colorToning.enabled
                && viewportFullParams.colorToning.autosat)) {
        reason = "auto-colour-toning";
        return false;
    }
    if (params->colorappearance.enabled) {
        reason = "cie";
        return false;
    }
    if (params->locallab.enabled) {
        reason = "local-lab";
        return false;
    }
    if (params->labCurve.contrast != 0) {
        reason = "lab-contrast";
        return false;
    }

    // Active full-frame filters would either be skipped here or re-applied
    // differently from the normal path, so leave them to the full pipeline.
    if (params->fattal.enabled || params->dehaze.enabled) {
        reason = "hdr-processing";
        return false;
    }
    if (params->spot.enabled && !params->spot.entries.empty()) {
        reason = "spot-removal";
        return false;
    }

    const ProcParams& last = viewportFullParams;

    if (params->raw != last.raw) {
        reason = "raw";
        return false;
    }
    if (params->wb != last.wb) {
        reason = "white-balance";
        return false;
    }
    if (params->icm != last.icm) {
        reason = "color-management";
        return false;
    }
    if (params->coarse != last.coarse) {
        reason = "coarse-transform";
        return false;
    }
    // The transform-stage parameters stay rejected until the ROI parity of
    // Crop::update() with a live transform change is validated on the GUI.
    if (params->commonTrans != last.commonTrans) {
        reason = "common-transform";
        return false;
    }
    if (params->rotate != last.rotate) {
        reason = "rotate";
        return false;
    }
    if (params->distortion != last.distortion) {
        reason = "distortion";
        return false;
    }
    if (params->lensProf != last.lensProf) {
        reason = "lens-profile";
        return false;
    }
    if (params->perspective != last.perspective) {
        reason = "perspective";
        return false;
    }
    if (params->gradient != last.gradient) {
        reason = "gradient";
        return false;
    }
    if (params->pcvignette != last.pcvignette) {
        reason = "post-crop-vignette";
        return false;
    }
    if (params->cacorrection != last.cacorrection) {
        reason = "ca-correction";
        return false;
    }
    if (params->vignetting != last.vignetting) {
        reason = "vignetting";
        return false;
    }
    if (params->spot != last.spot) {
        reason = "spot";
        return false;
    }
    if (params->filmNegative != last.filmNegative) {
        reason = "film-negative";
        return false;
    }
    if (params->dehaze != last.dehaze) {
        reason = "dehaze";
        return false;
    }
    if (params->fattal != last.fattal) {
        reason = "fattal";
        return false;
    }
    if (params->retinex != last.retinex) {
        reason = "retinex";
        return false;
    }
    if (params->pdsharpening != last.pdsharpening) {
        reason = "capture-sharpening";
        return false;
    }
    if (params->dirpyrDenoise != last.dirpyrDenoise) {
        reason = "denoise";
        return false;
    }
    if (params->cg != last.cg) {
        reason = "gamut-compression";
        return false;
    }
    if (params->locallab != last.locallab) {
        reason = "local-lab-params";
        return false;
    }
    if (!sameHighlightRecovery(params->toneCurve, last.toneCurve)) {
        reason = "highlight-recovery";
        return false;
    }
    if (params->toneCurve.autoexp != last.toneCurve.autoexp
            || params->toneCurve.histmatching
                != last.toneCurve.histmatching
            || params->blackwhite.autoc != last.blackwhite.autoc
            || params->colorToning.autosat != last.colorToning.autosat) {
        reason = "auto-flags";
        return false;
    }

    // The fast path rebuilds LUTs based on the request mask. If a LUT input
    // changed without its corresponding mask bit, fall back instead of
    // rendering with stale curves.
    const ProcParams& lastLut = viewportLutParams;

    if ((params->toneCurve != lastLut.toneCurve
            || params->rgbCurves != lastLut.rgbCurves
            || params->colorToning != lastLut.colorToning
            || params->blackwhite != lastLut.blackwhite)
            && !(todo & M_RGBCURVE)) {
        reason = "tone-curve-mask";
        return false;
    }
    if (params->labCurve != lastLut.labCurve
            && !(todo & M_LUMACURVE)) {
        reason = "lab-curve-mask";
        return false;
    }
    if (params->wavelet != lastLut.wavelet
            && !(todo & (M_LUMINANCE | M_COLOR))) {
        reason = "wavelet-mask";
        return false;
    }

    return true;
}

void ImProcCoordinator::prepareViewportLuts(int todo)
{
    if (todo & (M_RGBCURVE | M_CROP)) {
        // vhist16 is cached from the last completed full-frame pass and is a
        // function of the base image and working profile only.
        CurveFactory::complexCurve(
            params->toneCurve.expcomp, params->toneCurve.black / 65535.0,
            params->toneCurve.hlcompr, params->toneCurve.hlcomprthresh,
            params->toneCurve.shcompr, params->toneCurve.brightness,
            params->toneCurve.contrast, params->toneCurve.curve,
            params->toneCurve.curve2, vhist16, hltonecurve, shtonecurve,
            tonecurve, histToneCurve, customToneCurve1, customToneCurve2, 1);

        CurveFactory::RGBCurve(params->rgbCurves.rcurve, rCurve, 1);
        CurveFactory::RGBCurve(params->rgbCurves.gcurve, gCurve, 1);
        CurveFactory::RGBCurve(params->rgbCurves.bcurve, bCurve, 1);

        opautili = false;

        if (params->colorToning.enabled) {
            TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(
                params->icm.workingProfile);
            double wp[3][3] = {
                {wprof[0][0], wprof[0][1], wprof[0][2]},
                {wprof[1][0], wprof[1][1], wprof[1][2]},
                {wprof[2][0], wprof[2][1], wprof[2][2]}
            };
            params->colorToning.getCurves(ctColorCurve, ctOpacityCurve, wp,
                                          opautili);
            CurveFactory::diagonalCurve2Lut(params->colorToning.clcurve,
                                            clToningcurve,
                                            scale == 1 ? 1 : 16);
            CurveFactory::diagonalCurve2Lut(params->colorToning.cl2curve,
                                            cl2Toningcurve,
                                            scale == 1 ? 1 : 16);
        }

        if (params->blackwhite.enabled) {
            CurveFactory::curveBW(params->blackwhite.beforeCurve,
                                  params->blackwhite.afterCurve, vhist16bw,
                                  histToneCurveBW, beforeToneCurveBW,
                                  afterToneCurveBW, 1);
        }

        colourToningSatLimit = float(params->colorToning.satProtectionThreshold)
            / 100.f * 0.7f + 0.3f;
        colourToningSatLimitOpacity =
            1.f - (float(params->colorToning.saturatedOpacity) / 100.f);
    }

    if (todo & (M_LUMACURVE | M_CROP)) {
        // Lab contrast is restricted to zero by the eligibility check, so the
        // histogram cannot influence lumacurve. histLCurve is refreshed by the
        // deferred full pass. Guard the shared helper as well: a non-zero
        // contrast with the zero histogram would be visibly wrong.
        assert(params->labCurve.contrast == 0);
        if (params->labCurve.contrast != 0) {
            return;
        }
        CurveFactory::complexLCurve(
            params->labCurve.brightness, params->labCurve.contrast,
            params->labCurve.lcurve, viewportZeroLhist, lumacurve, histLCurve,
            scale == 1 ? 1 : 16, utili);
    }

    if (todo & M_LUMACURVE) {
        clcutili = CurveFactory::diagonalCurve2Lut(
            params->labCurve.clcurve, clcurve, scale == 1 ? 1 : 16);

        CurveFactory::complexsgnCurve(
            autili, butili, ccutili, cclutili, params->labCurve.acurve,
            params->labCurve.bcurve, params->labCurve.cccurve,
            params->labCurve.lccurve, chroma_acurve, chroma_bcurve, satcurve,
            lhskcurve, scale == 1 ? 1 : 16);
    }

    if (todo & (M_LUMINANCE | M_COLOR | M_AUTOEXP)) {
        wavcontlutili = CurveFactory::diagonalCurve2Lut(
            params->wavelet.wavclCurve, wavclCurve, scale == 1 ? 1 : 16);
    }

    updateViewportLutSnapshot(todo);
}

void ImProcCoordinator::updateViewportLutSnapshot(int todo)
{
    if (todo & (M_RGBCURVE | M_CROP)) {
        viewportLutParams.toneCurve = params->toneCurve;
        viewportLutParams.rgbCurves = params->rgbCurves;
        viewportLutParams.colorToning = params->colorToning;
        viewportLutParams.blackwhite = params->blackwhite;
    }
    if (todo & M_LUMACURVE) {
        // M_CROP rebuilds lumacurve but not clcurve/sgn curves, so it cannot
        // prove the whole Lab family current.
        viewportLutParams.labCurve = params->labCurve;
    }
    if (todo & (M_LUMINANCE | M_COLOR | M_AUTOEXP)) {
        viewportLutParams.wavelet = params->wavelet;
    }
}

void ImProcCoordinator::renderCrops(int todo, bool force)
{
    for (size_t i = 0; i < crops.size(); i++) {
        if (crops[i]->hasListener()
                && (force
                    || (todo & (M_MONITOR | M_RGBCURVE | M_LUMACURVE))
                    || crops[i]->get_skip() == 1)) {
            crops[i]->update(todo);     // may call ourselves
        }
    }
}

void ImProcCoordinator::ensureDeferredWatcher()
{
    paramsUpdateMutex.lock();

    if (!deferredWatcherStarted
            && !deferredWatcherStop.load(std::memory_order_acquire)) {
        deferredWatcherStarted = true;
        deferredWatcher = Glib::Thread::create(
            sigc::mem_fun(*this, &ImProcCoordinator::deferredWatcherLoop),
            0, true, true, Glib::THREAD_PRIORITY_LOW);
    }

    paramsUpdateMutex.unlock();
}

void ImProcCoordinator::deferredWatcherLoop()
{
    int sleepMs = VIEWPORT_DEFERRED_IDLE_POLL_MS;

    while (!deferredWatcherStop.load(std::memory_order_acquire)) {
        g_usleep(static_cast<gulong>(sleepMs) * 1000);

        if (deferredWatcherStop.load(std::memory_order_acquire)) {
            break;
        }

        bool due = false;
        {
            MyMutex::MyLock lock(paramsUpdateMutex);
            due = pendingDeferredMask != 0 && !deferredWatcherPaused
                && g_get_monotonic_time() >= pendingDeferredDeadlineUs;
            sleepMs = pendingDeferredMask != 0
                ? VIEWPORT_DEFERRED_POLL_MS : VIEWPORT_DEFERRED_IDLE_POLL_MS;
        }

        if (due) {
            flushDeferredAnalysis();
        }
    }
}

void ImProcCoordinator::flushDeferredAnalysis()
{
    int mask = 0;

    paramsUpdateMutex.lock();

    if (pendingDeferredMask == 0 || deferredWatcherPaused
            || deferredWatcherStop.load(std::memory_order_acquire)) {
        paramsUpdateMutex.unlock();
        return;
    }

    mask = pendingDeferredMask;
    pendingDeferredMask = 0;
    pendingDeferredHits = 0;
    pendingDeferredFirstUs = 0;
    pendingDeferredDeadlineUs = 0;
    forceFullPreview.store(true, std::memory_order_relaxed);
    changeSinceLast |= mask;

    paramsUpdateMutex.unlock();

    startProcessing();
}

void ImProcCoordinator::dropPendingDeferred()
{
    paramsUpdateMutex.lock();
    pendingDeferredMask = 0;
    pendingDeferredHits = 0;
    pendingDeferredFirstUs = 0;
    pendingDeferredDeadlineUs = 0;
    deferredCropReusable.store(false, std::memory_order_relaxed);
    paramsUpdateMutex.unlock();
}

void ImProcCoordinator::absorbPendingDeferred(int& todo)
{
    paramsUpdateMutex.lock();

    if (pendingDeferredMask != 0) {
        // The request that is about to run will rebuild the whole-frame
        // analysis, so it must also cover the stages the fast path skipped.
        todo |= pendingDeferredMask;
        pendingDeferredMask = 0;
        pendingDeferredHits = 0;
        pendingDeferredFirstUs = 0;
        pendingDeferredDeadlineUs = 0;
    }

    paramsUpdateMutex.unlock();
}

int ImProcCoordinator::queueViewportDeferredAnalysis(int todo,
                                                     unsigned request)
{
    // Rebuild the full-frame analysis from the base image already held in
    // oprevi. Base bits such as M_HDR/M_SPOT are over-approximated by several
    // event masks and their parameters are known to be unchanged here, so the
    // deferred pass only forces the monitor conversion and the curve/Lab
    // pipeline.
    const int deferred = viewportDeferredMask(todo);
    const gint64 now = g_get_monotonic_time();

    paramsUpdateMutex.lock();

    if (pendingDeferredMask == 0) {
        pendingDeferredFirstUs = now;
    }

    pendingDeferredMask |= deferred;
    ++pendingDeferredHits;

    // The crop rendered by this hit is reusable only while it is the only hit
    // of the burst; with accumulated hits the last render may not cover the
    // earlier stages, so the deferred pass renders the crops again.
    deferredCropReusable.store(viewportCropRenderReusable(pendingDeferredHits),
                               std::memory_order_relaxed);

    pendingDeferredDeadlineUs = viewportDeferredDeadline(
        pendingDeferredFirstUs, now, viewportDeferredSettleMs(),
        viewportDeferredMaxWaitMs());

    viewportDeferredParams = *params;
    viewportDeferredRequest.store(request, std::memory_order_relaxed);
    deferredCropRendered.store(true, std::memory_order_relaxed);

    const bool flushNow = viewportDeferredSettleMs() == 0;

    paramsUpdateMutex.unlock();

    if (flushNow) {
        flushDeferredAnalysis();
    } else {
        ensureDeferredWatcher();
    }

    return deferred;
}

void ImProcCoordinator::markViewportStateValid(bool highDetailNeeded)
{
    forceFullPreview.store(false, std::memory_order_relaxed);

    if (!viewportFastEnabled()) {
        viewportStateValid.store(false, std::memory_order_relaxed);
        return;
    }

    viewportHighDetail.store(highDetailNeeded, std::memory_order_relaxed);
    viewportStateValid.store(true, std::memory_order_relaxed);

    // A fast hit may have queued a newer deferred burst while this pass was
    // running. Only clear the per-hit bookkeeping when nothing is pending,
    // otherwise the newer burst would lose its suppression and request data.
    MyMutex::MyLock lock(paramsUpdateMutex);
    if (pendingDeferredMask == 0) {
        viewportDeferredRequest.store(0, std::memory_order_relaxed);
        deferredCropRendered.store(false, std::memory_order_relaxed);
        deferredCropReusable.store(false, std::memory_order_relaxed);
    }
}

void ImProcCoordinator::invalidateViewportState()
{
    viewportStateValid.store(false, std::memory_order_relaxed);
    viewportHighDetail.store(false, std::memory_order_relaxed);
    forceFullPreview.store(false, std::memory_order_relaxed);
    viewportDeferredRequest.store(0, std::memory_order_relaxed);
    deferredCropRendered.store(false, std::memory_order_relaxed);
    dropPendingDeferred();
}

DetailedCrop* ImProcCoordinator::createCrop(::EditDataProvider *editDataProvider, bool isDetailWindow)
{

    return new Crop(this, editDataProvider, isDetailWindow);
}


namespace
{

/*
 * Opt-in preview decision layer (RT_PREVIEW_GRAPH).
 *
 * The legacy path stays authoritative: this layer models the full-frame
 * preview as six blocks, marks them dirty from the event mask and the base
 * cache state, and asks StageGraph::plan() which blocks are allowed to run.
 * A pass whose plan would skip a block the legacy code executes fails closed:
 * the plan is rejected, the reason is logged, and the unchanged legacy block
 * sequence runs. No arithmetic, ordering or thread count is changed by this
 * layer.
 */
bool previewGraphEnabled()
{
    static const bool enabled = []() {
        const char* value = g_getenv("RT_PREVIEW_GRAPH");
        return value != nullptr && value[0] != '\0'
            && g_ascii_strcasecmp(value, "0") != 0
            && g_ascii_strcasecmp(value, "off") != 0;
    }();
    return enabled;
}

enum PreviewBlock {
    PREVIEW_BASE = 0,
    PREVIEW_ANALYSIS,
    PREVIEW_LOCAL,
    PREVIEW_RGB,
    PREVIEW_LAB,
    PREVIEW_OUTPUT,
    PREVIEW_BLOCK_COUNT
};

constexpr const char* PREVIEW_BLOCK_NAMES[PREVIEW_BLOCK_COUNT] = {
    "preview.base",
    "preview.analysis",
    "preview.local",
    "preview.rgb",
    "preview.lab",
    "preview.output"
};

/*
 * Bits that can start work in the analysis block. The set is deliberately a
 * superset of the legacy gates: extra dirt only makes the plan recompute a
 * block, while a missing bit would let the plan skip a block the legacy path
 * executes and is rejected by the validation below.
 */
constexpr int PREVIEW_ANALYSIS_BITS =
    M_PREPROC | M_RAW | M_WB | M_INIT | M_LINDENOISE | M_HDR | M_RETINEX
    | M_SPOT | M_CSHARP | M_AUTOEXP | M_HIGHQUAL | M_BLURMAP;

/*
 * Bits that can start work in the middle blocks. The local-adjustment region
 * runs as a whole inside a full-frame pass, so any processing bit dirtied
 * through ALL is treated as local work. RGB and Lab list their own tool bits
 * plus the upstream analysis bits that rebuild their inputs.
 */
constexpr int PREVIEW_LOCAL_BITS = ALL;
constexpr int PREVIEW_RGB_BITS =
    M_RGBCURVE | M_AUTOEXP | M_LUMACURVE | M_LUMINANCE | M_COLOR
    | M_TRANSFORM | M_HDR | M_SPOT | M_LINDENOISE | M_RETINEX;
constexpr int PREVIEW_LAB_BITS =
    M_LUMACURVE | M_LUMINANCE | M_COLOR | M_RGBCURVE | M_AUTOEXP
    | M_TRANSFORM | M_HDR | M_SPOT | M_LINDENOISE | M_RETINEX;

struct PreviewGraphResult {
    bool valid = true;
    const char* reason = nullptr;
    stage::StageAction action[PREVIEW_BLOCK_COUNT] = {};
};

PreviewGraphResult planPreviewGraph(int todo, bool panningRelatedChange,
                                    bool fullPipeline, bool baseCacheHit)
{
    PreviewGraphResult result;
    if (!previewGraphEnabled()) {
        return result;
    }

    stage::StageGraph graph;
    size_t nodes[PREVIEW_BLOCK_COUNT];
    for (int block = 0; block < PREVIEW_BLOCK_COUNT; ++block) {
        nodes[block] = graph.addNode(PREVIEW_BLOCK_NAMES[block],
                                     RT_STAGE_RECOMPUTE);
    }
    // Execution order of the full-frame preview pipeline.
    for (int block = 1; block < PREVIEW_BLOCK_COUNT; ++block) {
        graph.connect(nodes[block - 1], nodes[block]);
    }

    const auto mark = [&graph, &nodes](int block, bool dirty) {
        if (dirty) {
            graph.markDirty(nodes[block]);
        } else {
            graph.markClean(nodes[block]);
        }
    };

    // Base: rebuilt only when the base cache did not supply the image.
    mark(PREVIEW_BASE, fullPipeline && !baseCacheHit);
    // Analysis: preprocessing, demosaic, white balance and first analysis.
    mark(PREVIEW_ANALYSIS,
         fullPipeline && (todo & PREVIEW_ANALYSIS_BITS) != 0);
    mark(PREVIEW_LOCAL, fullPipeline && (todo & PREVIEW_LOCAL_BITS) != 0);
    mark(PREVIEW_RGB, fullPipeline && (todo & PREVIEW_RGB_BITS) != 0);
    mark(PREVIEW_LAB, fullPipeline && (todo & PREVIEW_LAB_BITS) != 0);
    // Output: monitor conversion and the analysis image.
    mark(PREVIEW_OUTPUT,
         panningRelatedChange || (todo & M_MONITOR) != 0);

    std::vector<size_t> order;
    if (!graph.topologicalOrder(order)
            || order.size() != PREVIEW_BLOCK_COUNT) {
        result.valid = false;
        result.reason = "cycle";
        return result;
    }

    const std::vector<stage::StageDecision> plan = graph.plan();
    if (plan.size() != PREVIEW_BLOCK_COUNT) {
        result.valid = false;
        result.reason = "incomplete-plan";
        return result;
    }
    for (int block = 0; block < PREVIEW_BLOCK_COUNT; ++block) {
        result.action[block] = plan[nodes[block]].action;
    }

    // Independent statement of what the legacy code executes. These mirror
    // the guards on the corresponding blocks, not the dirty rules above, so a
    // future change to either side is caught here.
    const bool legacyRuns[PREVIEW_BLOCK_COUNT] = {
        fullPipeline && !baseCacheHit,
        fullPipeline && (todo & PREVIEW_ANALYSIS_BITS) != 0,
        fullPipeline,
        fullPipeline,
        fullPipeline,
        panningRelatedChange || (todo & M_MONITOR) != 0
    };

    for (int block = 0; block < PREVIEW_BLOCK_COUNT; ++block) {
        if (legacyRuns[block]
                && result.action[block] == stage::StageAction::ReuseOutput) {
            result.valid = false;
            result.reason = PREVIEW_BLOCK_NAMES[block];
            return result;
        }
    }

    return result;
}

void logPreviewGraph(const PreviewGraphResult& result, int todo)
{
    static std::mutex mutex;
    static std::set<std::string> loggedFallbacks;
    static bool loggedActive = false;

    std::lock_guard<std::mutex> lock(mutex);

    if (!result.valid) {
        const std::string reason = result.reason != nullptr
                                       ? result.reason
                                       : "unknown";
        if (!loggedFallbacks.insert(reason).second) {
            return;
        }
        std::fprintf(stderr,
                     "preview-graph: fallback to legacy: plan would skip %s "
                     "but the legacy path runs it (todo=%d)\n",
                     reason.c_str(), todo);
        return;
    }

    if (loggedActive) {
        return;
    }
    loggedActive = true;
    std::fprintf(stderr,
                 "preview-graph: active, 6 blocks in execution order:");
    for (int block = 0; block < PREVIEW_BLOCK_COUNT; ++block) {
        std::fprintf(stderr, " %s=%s", PREVIEW_BLOCK_NAMES[block],
                     result.action[block] == stage::StageAction::Recompute
                         ? "recompute"
                         : "reuse");
    }
    std::fprintf(stderr, "\n");
}

} // namespace

namespace preview_dump
{

namespace
{

struct DumpState {
    bool enabled = false;
    std::string directory;
    uint64_t pass = 0;
    std::mutex mutex;
    bool writeFailed = false;
};

DumpState& storage()
{
    static DumpState dump;
    return dump;
}

DumpState& state()
{
    static std::once_flag once;
    std::call_once(once, []() {
        DumpState& dump = storage();
        const char* value = g_getenv("RT_PREVIEW_DUMP");
        if (value == nullptr || value[0] == '\0'
                || g_ascii_strcasecmp(value, "0") == 0
                || g_ascii_strcasecmp(value, "off") == 0) {
            return;
        }
        dump.directory = value;
        if (g_mkdir_with_parents(dump.directory.c_str(), 0700) != 0) {
            std::fprintf(stderr,
                         "preview-dump: cannot create the dump directory: "
                         "%s\n", g_strerror(errno));
            return;
        }
        dump.enabled = true;
    });
    return storage();
}

std::string dump_path(const std::string& directory, const char* name)
{
    char* path = g_build_filename(directory.c_str(), name, nullptr);
    std::string result = path != nullptr ? path : "";
    g_free(path);
    return result;
}

bool write_tiff(const std::string& path, const Image8* image)
{
    if (image == nullptr || image->getWidth() < 1 || image->getHeight() < 1) {
        return false;
    }
    // Uncompressed 16-bit keeps the dump byte-comparable between runs and
    // preserves every 8-bit sample exactly (v * 257).
    return image->saveTIFF(Glib::ustring(path), 16, false, true, false)
           == IMIO_SUCCESS;
}

bool write_text(const std::string& path, const std::string& body)
{
    std::ofstream out(path.c_str(),
                      std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out) {
        return false;
    }
    out << body;
    return out.good();
}

void report_failure()
{
    DumpState& dump = state();
    if (!dump.writeFailed) {
        dump.writeFailed = true;
        std::fprintf(stderr, "preview-dump: failed to write a dump file\n");
    }
}

} // namespace

bool enabled()
{
    return state().enabled;
}

void dump_full_frame(const Image8* preview, const Image8* work, int scale,
                     int todo)
{
    DumpState& dump = state();
    if (!dump.enabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(dump.mutex);
    if (preview == nullptr || work == nullptr) {
        return;
    }

    const uint64_t pass = ++dump.pass;
    const std::string previewPath = dump_path(dump.directory, "preview.tif");
    const std::string workPath = dump_path(dump.directory, "work.tif");

    if (!write_tiff(previewPath, preview) || !write_tiff(workPath, work)) {
        report_failure();
        return;
    }

    std::ostringstream json;
    json << "{\n"
         << "  \"schema\": \"rt-preview-dump/1\",\n"
         << "  \"pass\": " << pass << ",\n"
         << "  \"scale\": " << scale << ",\n"
         << "  \"todo\": " << todo << ",\n"
         << "  \"format\": \"uint16-tiff\",\n"
         << "  \"preview\": {\"file\": \"preview.tif\", \"width\": "
         << preview->getWidth() << ", \"height\": " << preview->getHeight()
         << ", \"channels\": 3},\n"
         << "  \"work\": {\"file\": \"work.tif\", \"width\": "
         << work->getWidth() << ", \"height\": " << work->getHeight()
         << ", \"channels\": 3}\n"
         << "}\n";

    if (!write_text(dump_path(dump.directory, "preview.json"), json.str())) {
        report_failure();
    }
}

void dump_crop(const Image8* crop, const char* tag, int visibleX,
               int visibleY, int visibleWidth, int visibleHeight, int skip)
{
    DumpState& dump = state();
    if (!dump.enabled || crop == nullptr || tag == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(dump.mutex);

    const std::string imageName = std::string("crop-") + tag + ".tif";
    const std::string jsonName = std::string("crop-") + tag + ".json";
    const std::string imagePath = dump_path(dump.directory, imageName.c_str());

    if (!write_tiff(imagePath, crop)) {
        report_failure();
        return;
    }

    std::ostringstream json;
    json << "{\n"
         << "  \"schema\": \"rt-preview-dump/1\",\n"
         << "  \"format\": \"uint16-tiff\",\n"
         << "  \"file\": \"" << imageName << "\",\n"
         << "  \"tag\": \"" << tag << "\",\n"
         << "  \"width\": " << crop->getWidth() << ",\n"
         << "  \"height\": " << crop->getHeight() << ",\n"
         << "  \"channels\": 3,\n"
         << "  \"visible\": {\"x\": " << visibleX << ", \"y\": "
         << visibleY << ", \"width\": " << visibleWidth << ", \"height\": "
         << visibleHeight << "},\n"
         << "  \"scale\": " << skip << "\n"
         << "}\n";

    if (!write_text(dump_path(dump.directory, jsonName.c_str()), json.str())) {
        report_failure();
    }
}

} // namespace preview_dump

// todo: bitmask containing desired actions, taken from changesSinceLast
void ImProcCoordinator::updatePreviewImage(int todo, bool panningRelatedChange,
                                           unsigned request)
{
    RT_PROFILE("proc::updatePreviewImage", PROCESS_IMAGE);

    // TODO Locallab printf
    MyMutex::MyLock processingLock(mProcessing);

    if (settings != nullptr && !settings->cacheDir.empty()) {
        previewBaseCache->setPersistentRoot(
            (settings->cacheDir + "/stages").raw());
    }

    // Keep the requested event mask for cache invalidation. The augmentation
    // below turns a crop-only change into a transform request, but the base
    // image itself does not change. Eligibility is evaluated on this
    // un-augmented mask so a derived transform does not force a full render.
    const int baseCacheTodo = todo;
    const int eligibilityTodo = todo;

    // This augmentation must happen before a stale request is requeued, since
    // the next request may only carry M_CROP and M_MONITOR.
    if ((todo & M_CROP) && ipf.needsPCVignetting()) {
        todo |= TRANSFORM;
    }

    if (previewRequest.load(std::memory_order_relaxed) != request) {
        paramsUpdateMutex.lock();
        changeSinceLast |= todo;
        if (panningRelatedChange) {
            changeSinceLast |= M_MONITOR;
        }
        paramsUpdateMutex.unlock();
        return;
    }

    PreviewTimer previewTimer(todo,
                              g_getenv("RT_PREVIEW_PROFILE") != nullptr);
    LoadProfile::Timer pipelineTimer(
        "preview pipeline", &LoadProfile::state().pipelineSeen);

    const auto prevdemo = App::get().options().prevdemo;
    ipf.setOpenCLAllowed(App::get().options().rtSettings.useOpenCL
                         && App::get().options().rtSettings.openCLMode == 1);
    opencl::setStageAllowed(App::get().options().rtSettings.useOpenCL
                            && App::get().options().rtSettings.openCLMode == 1);

    // Below 100% the Preview Demosaic Method preference is authoritative.
    // At 100% and above the per-tab FAST toggle decides. Detail windows use
    // the same shared base and do not override this choice.
    const bool prevdemoFull = prevdemo == PD_Sidecar;
    const bool fastPreview = fastPreviewEnabled.load(std::memory_order_acquire)
        && fastPreviewAllowed();
    const bool mainFull = mainPreviewFullResolution.load(
        std::memory_order_acquire);
    bool highDetailNeeded = needFullDemosaic(
        prevdemoFull, fastPreview, mainFull);
    //    printf("metwb=%s \n", params->wb.method.c_str());

    if (previewBaseCache->enabled()) {
        if (const char* reason = baseCacheTodoReason(baseCacheTodo)) {
            previewBaseCache->markDirty(reason);
        }
        if (highDetailStateKnown
                && highDetailNeeded != lastHighDetailNeeded) {
            previewBaseCache->markDirty("high-detail-transition");
        }
    }
    lastHighDetailNeeded = highDetailNeeded;
    highDetailStateKnown = true;

    previewTimer.viewportDeferred =
        forceFullPreview.load(std::memory_order_relaxed);

    const char* viewportFallback = nullptr;
    if (canUseViewportFastPath(eligibilityTodo, highDetailNeeded,
                               viewportFallback)) {
        int cropTodo = todo & VIEWPORT_CROP_TODO;
        if (cropTodo & (M_LUMACURVE | M_AUTOEXP)) {
            // Crop::update applies luminance/colour work for these bits only
            // through M_LUMINANCE/M_COLOR; keep the mask consistent.
            cropTodo |= M_LUMINANCE | M_COLOR;
        }

        prepareViewportLuts(eligibilityTodo);
        ipf.setScale(scale);

        const auto cropStart = std::chrono::steady_clock::now();
        renderCrops(cropTodo, true);
        const auto cropEnd = std::chrono::steady_clock::now();
        previewTimer.viewportFast = true;
        previewTimer.viewportCropMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                cropEnd - cropStart).count();
        previewTimer.viewportDeferredTodo =
            queueViewportDeferredAnalysis(eligibilityTodo, request);
        {
            MyMutex::MyLock lock(paramsUpdateMutex);
            previewTimer.viewportDeferredHits = pendingDeferredHits;
        }
        return;
    }

    previewTimer.viewportFallback = viewportFallback;

    // A request that bypasses the fast path must not leave a previous fast
    // state looking valid when its base inputs changed. If the base snapshot
    // is missing or differs, force the base/analysis stage even if the
    // original event mask did not include an init/HDR bit (for example a spot
    // reset or an image/scale transition).
    const bool stateWasValid =
        viewportStateValid.load(std::memory_order_relaxed);
    if (viewportFastEnabled()
            && (!stateWasValid || !sameBaseInputs(*params, viewportFullParams))) {
        viewportStateValid.store(false, std::memory_order_relaxed);
        // M_MONITOR also forces the full-frame block even when the original
        // event did not carry an init/HDR bit. The added bits make sure the
        // base image and analysis are rebuilt from the current parameters.
        todo |= M_INIT | M_HDR | M_MONITOR;
        if (params->spot != viewportFullParams.spot) {
            todo |= M_SPOT;
        }
        if (previewBaseCache->enabled()) {
            previewBaseCache->markDirty("viewport-base-refresh");
        }
    }

    // A request that takes the normal path satisfies any deferred analysis
    // work: it rebuilds the whole-frame pipeline, so the accumulated stages
    // must be part of its mask rather than left for a later pass.
    absorbPendingDeferred(todo);

    bool suppressDeferredCropRender = false;
    {
        MyMutex::MyLock lock(paramsUpdateMutex);
        suppressDeferredCropRender =
            deferredCropReusable.load(std::memory_order_relaxed)
            && deferredCropRendered.load(std::memory_order_relaxed)
            && viewportDeferredRequest.load(std::memory_order_relaxed) == request
            && *params == viewportDeferredParams;
    }

    bool fullPipelineRan = false;
    if (((todo & ALL) == ALL) || (todo & M_MONITOR) || panningRelatedChange || (highDetailNeeded && prevdemo != PD_Sidecar)) {
        fullPipelineRan = true;

        // Snapshot the request inputs before the pass normalises white balance
        // (and similar deterministic values). The GUI keeps the un-normalised
        // values, so comparing against the post-pass copy would reject every
        // subsequent request. The state only becomes valid once the pass ends.
        if (viewportFastEnabled()) {
            viewportFullParams = *params;
        }

        bwAutoR = bwAutoG = bwAutoB = -9000.f;

        RAWParams rp = params->raw;
        ColorManagementParams cmp = params->icm;
        LCurveParams  lcur = params->labCurve;
        bool spotsDone = false;

        if (!highDetailNeeded) {
            // if below 100% magnification, take a fast path
            if (rp.bayersensor.method != RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::NONE) && rp.bayersensor.method != RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::MONO)) {
                rp.bayersensor.method = RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::FAST);
            }

            //bayerrp.all_enhance = false;

            if (rp.xtranssensor.method != RAWParams::XTransSensor::getMethodString(RAWParams::XTransSensor::Method::NONE) && rp.xtranssensor.method != RAWParams::XTransSensor::getMethodString(RAWParams::XTransSensor::Method::MONO)) {
                rp.xtranssensor.method = RAWParams::XTransSensor::getMethodString(RAWParams::XTransSensor::Method::FAST);
            }

            rp.bayersensor.ccSteps = 0;
            rp.xtranssensor.ccSteps = 0;
            //rp.deadPixelFilter = rp.hotPixelFilter = false;
        }

        if (frameCountListener) {
            frameCountListener->FrameCountChanged(imgsrc->getFrameCount(), params->raw.bayersensor.imageNum);
        }

        // raw auto CA is bypassed if no high detail is needed, so we have to compute it when high detail is needed
        float reddeha = 0.f;//initialize black red, blue, green for dehaze
        float greendeha = 0.f;
        float bluedeha = 0.f;

        if ((todo & M_PREPROC) || (!highDetailPreprocessComputed && highDetailNeeded)) {
            previewBaseCache->markDirty("preprocess");
            imgsrc->setCurrentFrame(params->raw.bayersensor.imageNum);
            imgsrc->preprocess(rp, params->lensProf, params->coarse, reddeha, greendeha, bluedeha, true);
            if(imgsrc->getSensorType() == ST_BAYER) {//Bayer
                if (ablListener) {
                    if(rp.bayersensor.Dehablack) {
                        ablListener->autoBlackChanged(reddeha, greendeha, bluedeha);
                    }
                }
            } else if(imgsrc->getSensorType() == ST_FUJI_XTRANS) {//X trans
                if (ablxListener) {
                    if(rp.xtranssensor.Dehablackx) {
                        ablxListener->autoBlackxChanged(reddeha, greendeha, bluedeha);
                    }
                }
            }
            if (flatFieldAutoClipListener && rp.ff_AutoClipControl) {
                flatFieldAutoClipListener->flatFieldAutoClipValueChanged(imgsrc->getFlatFieldAutoClipValue());
            }

            imgsrc->getRAWHistogram(histRedRaw, histGreenRaw, histBlueRaw);
            hist_raw_dirty = !(hListener && hListener->updateHistogramRaw());

            highDetailPreprocessComputed = highDetailNeeded;
        }

        /*
        Demosaic is kicked off only when
        Detail considerations:
            accurate detail is not displayed yet needed based on preview specifics (driven via highDetailNeeded flag)
        OR
        HLR considerations:
            Color HLR alters rgb output of demosaic, so re-demosaic is needed when Color HLR is being turned off;
            if HLR is enabled and changing method *from* Color to any other method
            OR HLR gets disabled when Color method was selected
            If white balance changed with inpaint opposed, because inpaint opposed depends on the white balance
        */
        // If high detail (=100%) is newly selected, do a demosaic update, since the last was just with FAST

        if (imageTypeListener) {
            imageTypeListener->imageTypeChanged(imgsrc->isRAW(), imgsrc->getSensorType() == ST_BAYER, imgsrc->getSensorType() == ST_FUJI_XTRANS, imgsrc->isMono(), imgsrc->isGainMapSupported());
        }

        bool iscolor = (params->toneCurve.method == "Color" || params->toneCurve.method == "Coloropp");
        if ((todo & M_WB) && params->toneCurve.hrenabled && params->toneCurve.method == "Coloropp") {
            todo |= DEMOSAIC;
        }

        if ((todo & M_RAW)
                || (!highDetailRawComputed && highDetailNeeded)
                // || (params->toneCurve.hrenabled && params->toneCurve.method != "Color" && imgsrc->isRGBSourceModified())
                // || (!params->toneCurve.hrenabled && params->toneCurve.method == "Color" && imgsrc->isRGBSourceModified())) {
                || (params->toneCurve.hrenabled && !iscolor && imgsrc->isRGBSourceModified())
                || (!params->toneCurve.hrenabled && iscolor && imgsrc->isRGBSourceModified())) {
            if (settings->verbose) {
                if (imgsrc->getSensorType() == ST_BAYER) {
                    printf("Demosaic Bayer image n.%d using method: %s\n", rp.bayersensor.imageNum + 1, rp.bayersensor.method.c_str());
                } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS) {
                    printf("Demosaic X-Trans image with using method: %s\n", rp.xtranssensor.method.c_str());
                }
            }

            if (imgsrc->getSensorType() == ST_BAYER) {
                if (params->raw.bayersensor.method != RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::PIXELSHIFT)) {
                    imgsrc->setBorder(params->raw.bayersensor.border);
                } else {
                    imgsrc->setBorder(std::max(params->raw.bayersensor.border, 2));
                }
            } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS) {
                imgsrc->setBorder(params->raw.xtranssensor.border);
            }

            bool autoContrast = imgsrc->getSensorType() == ST_BAYER ? params->raw.bayersensor.dualDemosaicAutoContrast : params->raw.xtranssensor.dualDemosaicAutoContrast;
            double contrastThreshold = imgsrc->getSensorType() == ST_BAYER ? params->raw.bayersensor.dualDemosaicContrast : params->raw.xtranssensor.dualDemosaicContrast;
            previewBaseCache->markDirty("demosaic");
            imgsrc->demosaic(rp, autoContrast, contrastThreshold, params->pdsharpening.enabled);

            if (imgsrc->getSensorType() == ST_BAYER && bayerAutoContrastListener && autoContrast) {
                bayerAutoContrastListener->autoContrastChanged(contrastThreshold);
            } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS && xtransAutoContrastListener && autoContrast) {

                xtransAutoContrastListener->autoContrastChanged(contrastThreshold);
            }

            // if a demosaic happened we should also call getimage later, so we need to set the M_INIT flag
            todo |= (M_INIT | M_CSHARP);

        }

        if ((todo & (M_RAW | M_CSHARP)) && params->pdsharpening.enabled) {
            double pdSharpencontrastThreshold = params->pdsharpening.contrast;
            double pdSharpenRadius = params->pdsharpening.deconvradius;
            previewBaseCache->markDirty("capture-sharpening");
            imgsrc->captureSharpening(params->pdsharpening, sharpMask, pdSharpencontrastThreshold, pdSharpenRadius);

            if (pdSharpenAutoContrastListener && params->pdsharpening.autoContrast) {
                pdSharpenAutoContrastListener->autoContrastChanged(pdSharpencontrastThreshold);
            }

            if (pdSharpenAutoRadiusListener && params->pdsharpening.autoRadius) {
                pdSharpenAutoRadiusListener->autoRadiusChanged(pdSharpenRadius);
            }
        }


        if ((todo & M_RAW)
                || (!highDetailRawComputed && highDetailNeeded)
                //  || (params->toneCurve.hrenabled && params->toneCurve.method != "Color" && imgsrc->isRGBSourceModified())
                //  || (!params->toneCurve.hrenabled && params->toneCurve.method == "Color" && imgsrc->isRGBSourceModified())) {
                || (params->toneCurve.hrenabled && !iscolor && imgsrc->isRGBSourceModified())
                || (!params->toneCurve.hrenabled && iscolor && imgsrc->isRGBSourceModified())) {
            if (highDetailNeeded) {
                highDetailRawComputed = true;
            } else {
                highDetailRawComputed = false;
            }

            if (params->retinex.enabled) {
                lhist16RETI(32768);
                lhist16RETI.clear();

                imgsrc->retinexPrepareBuffers(params->icm, params->retinex, conversionBuffer, lhist16RETI);
            }
        }

        if (todo & (M_INIT | M_LINDENOISE | M_HDR)) {
            if (params->wb.method == "autitcgreen") {
                imgsrc->getrgbloc(0, 0, fh, fw, 0, 0, fh, fw, params->wb);
            }
        }

        if ((todo & (M_RETINEX | M_INIT)) && params->retinex.enabled) {
            bool dehacontlutili = false;
            bool mapcontlutili = false;
            bool useHsl = false;
            LUTf cdcurve(65536, 0);
            LUTf mapcurve(65536, 0);

            imgsrc->retinexPrepareCurves(params->retinex, cdcurve, mapcurve, dehatransmissionCurve, dehagaintransmissionCurve, dehacontlutili, mapcontlutili, useHsl, lhist16RETI, histLRETI);
            float minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax;
            imgsrc->retinex(params->icm, params->retinex,  params->toneCurve, cdcurve, mapcurve, dehatransmissionCurve, dehagaintransmissionCurve, conversionBuffer, dehacontlutili, mapcontlutili, useHsl, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax, histLRETI);   //enabled Retinex

            if (dehaListener) {
                dehaListener->minmaxChanged(maxCD, minCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax);
            }
        }

       // const bool autowb = (params->wb.method == "autold" || params->wb.method == "autitcgreen");

        if (settings->verbose) {
            printf("automethod=%s \n", params->wb.method.c_str());
        }

        if (todo & (M_INIT | M_LINDENOISE | M_HDR)) {
            MyMutex::MyLock initLock(minit);  // Also used in crop window
            //imgsrc->HLRecovery_Global(params->toneCurve);   // this handles Color HLRecovery


            if (settings->verbose) {
                printf("Applying white balance, color correction & sRBG conversion...\n");
            }

            currWB = ColorTemp(params->wb.temperature, params->wb.green, params->wb.equal, params->wb.method, params->wb.observer);
            int tempnotisraw = 6501;//D65 with Observer 2° - 6473 with Observer 10°
            double greennotisraw = 1.;//D65 with Observer 2° - 0.967 with Observer 10°

            if (!imgsrc->isRAW() && params->wb.method == "autitcgreen") {
                if (params->wb.compat_version == 1) {
                    // ITCWB compatibility version 1 used 5000 K and observer 10
                    // degrees for non-raw files.
                    auto currWBitc = ColorTemp(5000., 1., 1., params->wb.method, StandardObserver::TEN_DEGREES);
                    currWBitc = currWBitc.convertObserver(params->wb.observer);
                    tempnotisraw = currWBitc.getTemp();
                    greennotisraw = currWBitc.getGreen();
                } else {
                    auto currWBitc = imgsrc->getWB();//if jpg TIF with another illuminant
                    currWBitc = currWBitc.convertObserver(params->wb.observer);
                    tempnotisraw = currWBitc.getTemp();
                    greennotisraw = currWBitc.getGreen();
                }
            }

            int dread = 0;
            int bia = 1;
            float studgood = 1000.f;
            int nocam = 0;
            int kcam = 0;
            float minchrom = 1000.f;
            float delta = 0.f;
            int kmin  = 20;
            float minhist = 1000000000.f;
            float maxhist = -1000.f;
            double greenitc = 1.;
            float temp0 = 5000.f;
            bool extra = false;
            bool forcewbgrey = false;

            if (!params->wb.enabled) {
                currWB = ColorTemp();
            } else if (params->wb.method == "Camera") {
                currWB = imgsrc->getWB();
                lastAwbauto = ""; //reinitialize auto
                
            } else if (params->wb.method == "autold") {
                 if (lastAwbEqual != params->wb.equal || lastAwbTempBias != params->wb.tempBias || lastAwbauto != params->wb.method) {
                    double rm, gm, bm;
                    if (params->wb.compat_version == 1 && !imgsrc->isRAW()) {
                        // RGB grey compatibility version 1 used the identity
                        // multipliers plus temperature bias for non-raw files.
                        rm = gm = bm = 1.;
                    } else {
                        imgsrc->getAutoWBMultipliers(rm, gm, bm);
                    }

                    if (rm != -1.) {
                        double bias = params->wb.tempBias;

                        autoWB.update(rm, gm, bm, params->wb.equal, params->wb.observer, bias);
                        lastAwbEqual = params->wb.equal;
                        lastAwbObserver = params->wb.observer;
                        lastAwbTempBias = params->wb.tempBias;
                        lastAwbauto = params->wb.method;
                    } else {
                        lastAwbEqual = -1.;
                        lastAwbObserver = ColorTemp::DEFAULT_OBSERVER;
                        lastAwbTempBias = 0.0;
                        lastAwbauto = "";
                        autoWB.useDefaults(params->wb.equal, params->wb.observer);
                        
                    } 

                    //double rr,gg,bb;
                    //autoWB.getMultipliers(rr,gg,bb);
                }
                currWB = autoWB;               
               // lastAwbauto = ""; //reinitialize auto
            } else if (params->wb.method == "autitcgreen") { //(// autowb) {
                double rm;
                double gm;
                double bm;
                imgsrc->getAutoWBMultipliersItcGreen(
                    *params,
                    forcewbgrey,
                    kcam,
                    greenitc,
                    extra,
                    temp0,
                    delta,
                    bia,
                    dread,
                    nocam,
                    studgood,
                    minchrom,
                    kmin,
                    minhist,
                    maxhist,
                    fh,
                    fw,
                    currWB,
                    tempnotisraw,
                    greennotisraw,
                    lastAwbEqual == params->wb.equal && lastAwbObserver == params->wb.observer && lastAwbTempBias == params->wb.tempBias && lastAwbauto == params->wb.method,
                    autoWB,
                    rm,
                    gm,
                    bm);

                if (imgsrc->isRAW() || lastAwbEqual != params->wb.equal || lastAwbObserver != params->wb.observer || lastAwbTempBias != params->wb.tempBias || lastAwbauto != params->wb.method) {
                    if (rm != -1.) {
                        autoWB.update(rm, gm, bm, params->wb.equal, params->wb.observer);
                        lastAwbEqual = params->wb.equal;
                        lastAwbObserver = params->wb.observer;
                        lastAwbTempBias = params->wb.tempBias;
                        lastAwbauto = params->wb.method;
                    } else {
                        lastAwbEqual = -1.;
                        lastAwbObserver = ColorTemp::DEFAULT_OBSERVER;
                        lastAwbTempBias = 0.0;
                        lastAwbauto = "";
                        autoWB.useDefaults(params->wb.equal, params->wb.observer);
                    }
                }

                currWB = autoWB;
            }

            double rw = 1.;
            double gw = 1.;
            double bw = 1.;
            if (params->wb.enabled) {
                currWB = currWB.convertObserver(params->wb.observer);
                params->wb.temperature = static_cast<int>(currWB.getTemp());
                params->wb.green = currWB.getGreen();

                currWB.getMultipliers(rw, gw, bw);
                imgsrc->wbMul2Camera(rw, gw, bw);
  //              params->wb.itcwb_sampling = false;
                /*
                printf("ra=%f ga=%f ba=%f\n", rw, gw, bw);
                //recalculate temp and green with wb multipliers.
                imgsrc->wbCamera2Mul(rw, gw, bw);
                ColorTemp ct(rw, gw, bw, 1.0, currWB.getObserver());
                //allows to calculate temp and green with multipliers in case of we want in GUI
                float tem = ct.getTemp();
                float gre  = ct.getGreen();
                printf("tem=%f gre=%f \n", (double) tem, (double) gre);
                */
            }

            int met = 0;

            if (awbListener && params->wb.enabled) {
                if (params->wb.method ==  "autitcgreen"  && imgsrc->isRAW()) {//Raw files
                    if (params->wb.itcwb_sampling) {
                        dread = 1;
                        studgood = 1.f;
                        awbListener->WBChanged(met, params->wb.temperature, params->wb.green, rw, gw, bw, 0, 1, 0, dread, studgood, 0, 0, 0, 0, AutoWBListener::AWBMode::TEMP_CORRELATION_RAW);

                    } else {
                        minchrom = LIM(minchrom, 0.f, 0.9f);
                        delta = LIM(delta, 0.f, 0.9f);
                        minhist = std::max(minhist, 100.f);
                        maxhist = std::max(maxhist, 1000.f);
                        kmin = std::max(kmin, 18);
                        dread = LIM(dread, 10, 239);
                        awbListener->WBChanged(met, params->wb.temperature, params->wb.green, rw, gw, bw, temp0, delta, bia, dread, studgood, minchrom, kmin, minhist, maxhist, AutoWBListener::AWBMode::TEMP_CORRELATION_RAW);
                    }
                } else if (params->wb.method ==  "autitcgreen"  && !imgsrc->isRAW()) {//non raw files
                    params->wb.temperature = tempnotisraw;
                    params->wb.green = greennotisraw;
                    currWB = ColorTemp(params->wb.temperature, params->wb.green, params->wb.equal, params->wb.method, params->wb.observer);

                    awbListener->WBChanged(met, params->wb.temperature, params->wb.green, rw, gw, bw, -1.f,  -1.f, 1, 1, -1.f, -1.f, 1, -1.f, -1.f, AutoWBListener::AWBMode::TEMP_CORRELATION_NON_RAW);//false => hide settings

                } else if (params->wb.method == "autold"){
                    awbListener->WBChanged(met, params->wb.temperature, params->wb.green, rw, gw, bw, -1.f,  -1.f, 1, 1, -1.f, -1.f, 1, -1.f, -1.f, AutoWBListener::AWBMode::RGB_GREY);
                } else {
                    awbListener->WBChanged(met, params->wb.temperature, params->wb.green, rw, gw, bw, -1.f,  -1.f, 1, 1, -1.f, -1.f, 1, -1.f, -1.f, AutoWBListener::AWBMode::NONE);
                }
            }

            /*
                    GammaValues g_a;
                    double pwr = 1.0 / params->icm.gampos;
                    double ts = params->icm.slpos;


                    int mode = 0;
                    Color::calcGamma(pwr, ts, mode, g_a); // call to calcGamma with selected gamma and slope
                        printf("ga[0]=%f ga[1]=%f ga[2]=%f ga[3]=%f ga[4]=%f\n", g_a[0],g_a[1],g_a[2],g_a[3],g_a[4]);

                        Glib::ustring datal;
                        datal = "lutsrgb.txt";
                                ofstream fou(datal, ios::out | ios::trunc);

                    for(int i=0; i < 212; i++) {
                        //printf("igamma2=%i\n", (int) 65535.f*Color::igamma2(i/212.0));
                                float gam = Color::igamma2(i/211.0);
                                int lutga = nearbyint(65535.f* gam);
                              //  fou << 65535*(int)Color::igamma2(i/212.0) << endl;
                                fou << i << " " << lutga << endl;

                    }
                            fou.close();
            */
            int tr = getCoarseBitMask(params->coarse);

            imgsrc->getFullSize(fw, fh, tr);

            // Will (re)allocate the preview's buffers. Preserve the
            // deliberately small initial-preview policy across this call.
            setScale(scale, initialRoughPreview);
            PreviewProps pp(0, 0, fw, fh, scale);
            const bool baseCacheFullFrame =
                pp.getX() == 0 && pp.getY() == 0
                && pp.getWidth() == fw && pp.getHeight() == fh
                && pp.getSkip() == scale;
            // Tells to the ImProcFunctions' tools what the preview scale is,
            // which may lead to some simplifications.
            ipf.setScale(scale);

            const bool baseCacheEnabled = previewBaseCache->enabled();

            const auto makeBaseCacheKey = [&]() {
                PreviewBaseCache::Key key;
                key.source = imgsrc;

                // Stable identity for the persistent store. The pointer and
                // the transient generation counter are process-local.
                std::string identity = imgsrc->getFileName().raw();
                struct stat fileStat {};
                if (!identity.empty()
                        && ::stat(identity.c_str(), &fileStat) == 0) {
                    identity += ":" + std::to_string(fileStat.st_size)
                              + ":" + std::to_string(fileStat.st_mtime);
                }
                key.sourceIdentity = identity;

                key.frame = params->raw.bayersensor.imageNum;
                key.sensorType = static_cast<int>(imgsrc->getSensorType());
                key.rotateDegree = imgsrc->getRotateDegree();
                key.raw = imgsrc->isRAW();

                key.wbTemp = currWB.getTemp();
                key.wbGreen = currWB.getGreen();
                key.wbEqual = currWB.getEqual();
                key.wbObserver = currWB.getObserver();

                key.highDetailNeeded = highDetailNeeded;
                key.highDetailPreprocessComputed =
                    highDetailPreprocessComputed;
                key.highDetailRawComputed = highDetailRawComputed;

                key.bayerMethod = params->raw.bayersensor.method;
                key.bayerCcSteps = params->raw.bayersensor.ccSteps;
                key.xtransMethod = params->raw.xtranssensor.method;
                key.xtransCcSteps = params->raw.xtranssensor.ccSteps;
                key.rawExposure = params->raw.expos;

                key.hrEnabled = params->toneCurve.hrenabled;
                key.hrMethod = params->toneCurve.method;
                key.hrBlend = params->toneCurve.hlbl;
                key.hrThreshold = params->toneCurve.hlth;
                key.clampOOG = params->toneCurve.clampOOG;

                key.rgbSourceModified = imgsrc->isRGBSourceModified();
                key.sourceGeneration = imgsrc->getSourceGeneration();

                key.transform = tr;
                key.scale = scale;
                key.fullWidth = fw;
                key.fullHeight = fh;
                key.width = pW;
                key.height = pH;
                return key;
            };

            std::string baseCacheBypass;
            bool baseCacheHit = false;
            PreviewBaseCache::Key baseCacheKey;

            if (baseCacheEnabled) {
                if (initialRoughPreview) {
                    baseCacheBypass = "initial-rough";
                } else if (params->spot.enabled) {
                    baseCacheBypass = "spot";
                } else if (params->filmNegative.enabled) {
                    baseCacheBypass = "film-negative";
                } else if (params->dehaze.enabled) {
                    baseCacheBypass = "dehaze";
                } else if (params->fattal.enabled) {
                    baseCacheBypass = "fattal";
                } else if (params->retinex.enabled) {
                    // Retinex modifies the shared raw planes before getImage().
                    baseCacheBypass = "retinex";
                } else if (!baseCacheFullFrame) {
                    baseCacheBypass = "roi";
                } else if (!PreviewBaseCache::fits(pW, pH)) {
                    baseCacheBypass = "too-large";
                }

                if (!baseCacheBypass.empty()) {
                    previewBaseCache->noteBypass(baseCacheBypass);
                } else {
                    baseCacheKey = makeBaseCacheKey();
                    std::string missReason;
                    baseCacheHit = previewBaseCache->tryRestore(
                        baseCacheKey, orig_prev, missReason);
                }
            }

            if (!baseCacheHit) {
                {
                    RT_PROFILE("preview::base_image", PROCESS_IMAGE);
                    imgsrc->getImage(currWB, tr, orig_prev, pp,
                                     params->toneCurve, params->raw);
                }

                if (baseCacheEnabled && baseCacheBypass.empty()
                        && !initialRoughPreview) {
                    // Capture the key immediately after getImage(). HLR can
                    // mutate the shared source during getImage(), so the
                    // store key must use the post-call generation.
                    const PreviewBaseCache::Key storeKey = makeBaseCacheKey();
                    previewBaseCache->store(storeKey, orig_prev);
                }
            }

            // The base image now matches the decision computed above, either
            // rebuilt or restored from the base cache, so record the
            // preference it was built with. Passes that reuse the base,
            // including the viewport fast path, must not update this.
            lastPassPrevdemoBase.store(
                prevdemoFull ? PrevdemoBase::SIDECAR : PrevdemoBase::FAST,
                std::memory_order_release);

            previewTimer.cacheHit = baseCacheHit;

            // The base cache state is known now, so the decision layer can be
            // evaluated for the blocks that follow. A rejected plan never
            // changes execution: the legacy block sequence below is the only
            // implementation, and the log records why the plan was refused.
            if (previewGraphEnabled()) {
                const auto graphBegin = std::chrono::steady_clock::now();
                const PreviewGraphResult decision = planPreviewGraph(
                    todo, panningRelatedChange, fullPipelineRan, baseCacheHit);
                const auto graphEnd = std::chrono::steady_clock::now();
                stage::profile_duration(
                    "preview.graph.decide",
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            graphEnd - graphBegin)
                            .count()));
                logPreviewGraph(decision, todo);
            }

            if ((todo & M_SPOT) && params->spot.enabled && !params->spot.entries.empty()) {
                spotsDone = true;
                PreviewProps pp(0, 0, fw, fh, scale);
                ipf.removeSpots(orig_prev, imgsrc, params->spot.entries, pp, currWB, nullptr, tr);
            }

            denoiseInfoStore.valid = false;
            //ColorTemp::CAT02 (orig_prev, &params) ;
            //   printf("orig_prevW=%d\n  scale=%d",orig_prev->width, scale);
            /* Issue 2785, disabled some 1:1 tools
                    if (todo & M_LINDENOISE) {
                        DirPyrDenoiseParams denoiseParams = params->dirpyrDenoise;
                        if (denoiseParams.enabled && (scale==1)) {
                            Imagefloat *calclum = NULL ;

                            denoiseParams.getCurves(noiseLCurve,noiseCCurve);
                            int nbw=6;//nb tile W
                            int nbh=4;//

                            float ch_M[nbw*nbh];
                            float max_r[nbw*nbh];
                            float max_b[nbw*nbh];

                            if (denoiseParams.Lmethod == "CUR") {
                                if (noiseLCurve)
                                    denoiseParams.luma = 0.5f;
                                else
                                    denoiseParams.luma = 0.0f;
                            } else if (denoiseParams.Lmethod == "SLI")
                                noiseLCurve.Reset();


                            if (noiseLCurve || noiseCCurve){//only allocate memory if enabled and scale=1
                                // we only need image reduced to 1/4 here
                                calclum = new Imagefloat ((pW+1)/2, (pH+1)/2);//for luminance denoise curve
                                for(int ii=0;ii<pH;ii+=2){
                                    for(int jj=0;jj<pW;jj+=2){
                                        calclum->r(ii>>1,jj>>1) = orig_prev->r(ii,jj);
                                        calclum->g(ii>>1,jj>>1) = orig_prev->g(ii,jj);
                                        calclum->b(ii>>1,jj>>1) = orig_prev->b(ii,jj);
                                    }
                                }
                                imgsrc->convertColorSpace(calclum, params->icm, currWB);//calculate values after colorspace conversion
                            }

                            int kall=1;
                            ipf.RGB_denoise(kall, orig_prev, orig_prev, calclum, ch_M, max_r, max_b, imgsrc->isRAW(), denoiseParams, imgsrc->getDirPyrDenoiseExpComp(), noiseLCurve, noiseCCurve, chaut, redaut, blueaut, maxredaut, maxblueaut, nresi, highresi);
                        }
                    }
            */

            if (params->filmNegative.enabled) {

                // Process film negative AFTER colorspace conversion
                if (params->filmNegative.colorSpace != FilmNegativeParams::ColorSpace::INPUT) {
                    imgsrc->convertColorSpace(orig_prev, params->icm, currWB);
                }

                // Perform negative inversion. If needed, upgrade filmNegative params for backwards compatibility with old profiles
                if (ipf.filmNegativeProcess(orig_prev, orig_prev, params->filmNegative, params->raw, imgsrc, currWB) && filmNegListener) {
                    filmNegListener->filmRefValuesChanged(params->filmNegative.refInput, params->filmNegative.refOutput);
                }

                // Process film negative BEFORE colorspace conversion (legacy mode)
                if (params->filmNegative.colorSpace == FilmNegativeParams::ColorSpace::INPUT) {
                    imgsrc->convertColorSpace(orig_prev, params->icm, currWB);
                }

            } else {
                imgsrc->convertColorSpace(orig_prev, params->icm, currWB);
            }

             if (params->cg.enabled) {//gamut compression
                float mac = 0.f;
                float mac0 = 0.f;
                float mac1 = 0.f;
                float mac2 = 0.f;
                bool auto_dc = params->cg.autodc;
                bool auto_dm = params->cg.autodm;
                bool auto_dy = params->cg.autody;
                int beginend = 0;
                int nbsegam = 0;
                float powe = 1.f;
                ipf.gamutcompr(orig_prev, orig_prev, beginend, powe, nbsegam, mac, mac0, mac1, mac2);
                if (acmaxListener) {
                   acmaxListener->achromaticChanged((double) mac, mac0, mac1, mac2, auto_dc, auto_dm, auto_dy);
                }
            }


            ipf.firstAnalysis(orig_prev, *params, vhist16);
        }
  
        oprevi = orig_prev;

        if ((todo & M_SPOT) && !spotsDone) {
            if (params->spot.enabled && !params->spot.entries.empty()) {
                allocCache(spotprev);
                orig_prev->copyData(spotprev);
                PreviewProps pp(0, 0, fw, fh, scale);
                ipf.removeSpots(spotprev, imgsrc, params->spot.entries, pp, currWB, &params->icm, tr);
            } else {
                if (spotprev) {
                    delete spotprev;
                    spotprev = nullptr;
                }
            }
        }

        if (spotprev) {
            spotprev->copyData(orig_prev);
        }

        if ((todo & M_HDR) && (params->fattal.enabled || params->dehaze.enabled)) {
            if (fattal_11_dcrop_cache) {
                delete fattal_11_dcrop_cache;
                fattal_11_dcrop_cache = nullptr;
            }

            ipf.dehaze(orig_prev, params->dehaze);
            ipf.ToneMapFattal02(orig_prev, params->fattal, 3, 0, nullptr, 0, 0, 0, false);

            if (oprevi != orig_prev) {
                delete oprevi;
            }
        }

        // Remove transformation if unneeded
        bool needstransform = ipf.needsTransform(fw, fh, imgsrc->getRotateDegree(), imgsrc->getMetaData());

        const bool cam02 = params->colorappearance.modelmethod == "02" && params->colorappearance.enabled;
      //  if ((needstransform || ((todo & (M_TRANSFORM | M_RGBCURVE))  && params->dirpyrequalizer.cbdlMethod == "bef" && params->dirpyrequalizer.enabled && !params->colorappearance.enabled))) {
        if ((needstransform || ((todo & (M_TRANSFORM | M_RGBCURVE))  && params->dirpyrequalizer.cbdlMethod == "bef" && params->dirpyrequalizer.enabled && !cam02))) {
            // Forking the image
            assert(oprevi);
            Imagefloat *op = oprevi;
            oprevi = new Imagefloat(pW, pH);

            if (needstransform)
                ipf.transform(op, oprevi, 0, 0, 0, 0, pW, pH, fw, fh,
                              imgsrc->getMetaData(), imgsrc->getRotateDegree(), false);
            else {
                op->copyData(oprevi);
            }
        }

        for (int sp = 0; sp < (int)params->locallab.spots.size(); sp++) {
            if(params->locallab.spots.at(sp).expsharp  && params->dirpyrequalizer.cbdlMethod == "bef") {
                if(params->locallab.spots.at(sp).shardamping < 1) {
                    params->locallab.spots.at(sp).shardamping = 1;
                }
            }
        }

     //   if ((todo & (M_TRANSFORM | M_RGBCURVE))  && params->dirpyrequalizer.cbdlMethod == "bef" && params->dirpyrequalizer.enabled && !params->colorappearance.enabled) {
        if ((todo & (M_TRANSFORM | M_RGBCURVE))  && params->dirpyrequalizer.cbdlMethod == "bef" && params->dirpyrequalizer.enabled && !cam02) {
            const int W = oprevi->getWidth();
            const int H = oprevi->getHeight();
            LabImage labcbdl(W, H);
            ipf.rgb2lab(*oprevi, labcbdl, params->icm.workingProfile);
            ipf.dirpyrequalizer(&labcbdl, scale);
            ipf.lab2rgb(labcbdl, *oprevi, params->icm.workingProfile);
        }

        if (todo & M_AUTOEXP) {
            if (params->toneCurve.autoexp) {
                LUTu aehist;
                int aehistcompr;
                imgsrc->getAutoExpHistogram(aehist, aehistcompr);
                ipf.getAutoExp(aehist, aehistcompr, params->toneCurve.clip, params->toneCurve.expcomp,
                               params->toneCurve.brightness, params->toneCurve.contrast, params->toneCurve.black, params->toneCurve.hlcompr, params->toneCurve.hlcomprthresh);

                if (aeListener)
                    aeListener->autoExpChanged(params->toneCurve.expcomp, params->toneCurve.brightness, params->toneCurve.contrast,
                                               params->toneCurve.black, params->toneCurve.hlcompr, params->toneCurve.hlcomprthresh, params->toneCurve.hrenabled);
            }

            if (params->toneCurve.histmatching ) {
                bool exectrcexp = false;//store if Abstract profile enabled
                exectrcexp = params->icm.trcExp;
                if (!params->toneCurve.fromHistMatching) {
                    if(params->icm.trcExp) {
                        params->icm.trcExp = false;//disabled Abstract profile, if hismatching
                    }
                    imgsrc->getAutoMatchedToneCurve(params->icm, params->raw, params->wb.observer, params->toneCurve.curve);
                    params->icm.trcExp = exectrcexp;//restore Abstract profile 
                }

                if (params->toneCurve.autoexp) {
                    params->toneCurve.expcomp = 0.0;
                }

                params->toneCurve.autoexp = false;
                params->toneCurve.curveMode = ToneCurveMode::FILMLIKE;
                params->toneCurve.curve2 = { 0 };
                params->toneCurve.brightness = 0;
                params->toneCurve.contrast = 0;
                params->toneCurve.black = 0;
                params->toneCurve.fromHistMatching = true;

                if (aeListener) {
                    aeListener->autoMatchedToneCurveChanged(params->toneCurve.curveMode, params->toneCurve.curve);
                }
            }

            // Encoding log with locallab and SE capture sharpening
            if (params->locallab.enabled && !params->locallab.spots.empty()) {
                const int sizespot = (int)params->locallab.spots.size();
                const LocallabParams::LocallabSpot defSpot;
                std::vector<LocallabListener::locallabcieBEF> locallciebef;
                std::vector<LocallabListener::locallabsharBEF> locallsharbef;

                float *sourceg = nullptr;
                sourceg = new float[sizespot];
                float *sourceab = nullptr;
                sourceab = new float[sizespot];
                float *targetg = nullptr;
                targetg = new float[sizespot];
                bool *log = nullptr;
                log = new bool[sizespot];
                bool *cie = nullptr;
                cie = new bool[sizespot];
                bool *shar = nullptr;
                shar = new bool[sizespot];
                bool *autocomput = nullptr;
                autocomput = new bool[sizespot];
                float *blackev = nullptr;
                blackev = new float[sizespot];
                float *whiteev = nullptr;
                whiteev = new float[sizespot];
                bool *Autogr = nullptr;
                Autogr = new bool[sizespot];
                bool *autocie = nullptr;
                autocie = new bool[sizespot];
                int *whits = nullptr;
                whits = new int[sizespot];
                int *blacks = nullptr;
                blacks = new int[sizespot];
                int *whitslog = nullptr;
                whitslog = new int[sizespot];
                int *blackslog = nullptr;
                blackslog = new int[sizespot];
                bool *blackredu = nullptr;
                blackredu = new bool[sizespot];
                bool *autoradius = nullptr;
                autoradius = new bool[sizespot];
                float *caprad = nullptr;
                caprad = new float[sizespot];

                float *locx = nullptr;
                locx = new float[sizespot];
                float *locy = nullptr;
                locy = new float[sizespot];
                float *locxL = nullptr;
                locxL = new float[sizespot];
                float *locyT = nullptr;
                locyT = new float[sizespot];
                float *centx = nullptr;
                centx = new float[sizespot];
                float *centy = nullptr;
                centy = new float[sizespot];

                for (int sp = 0; sp < sizespot; sp++) {
                    log[sp] = params->locallab.spots.at(sp).explog;
                    cie[sp] = params->locallab.spots.at(sp).expcie;
                    shar[sp] = params->locallab.spots.at(sp).expsharp;
                    autocomput[sp] = params->locallab.spots.at(sp).autocompute;
                    autocie[sp] = params->locallab.spots.at(sp).Autograycie;
                    blackev[sp] = params->locallab.spots.at(sp).blackEv;
                    whiteev[sp] = params->locallab.spots.at(sp).whiteEv;
                    sourceg[sp] = params->locallab.spots.at(sp).sourceGray;
                    sourceab[sp] = params->locallab.spots.at(sp).sourceabs;
                    whits[sp] = params->locallab.spots.at(sp).whitescie;
                    blacks[sp] = params->locallab.spots.at(sp).blackscie;
                    whitslog[sp] = params->locallab.spots.at(sp).whiteslog;
                    blackslog[sp] = params->locallab.spots.at(sp).blackslog;
                    Autogr[sp] = params->locallab.spots.at(sp).Autogray;
                    targetg[sp] = params->locallab.spots.at(sp).targetGray;
                    locx[sp] = params->locallab.spots.at(sp).loc.at(0) / 2000.0;
                    locy[sp] = params->locallab.spots.at(sp).loc.at(2) / 2000.0;
                    locxL[sp] = params->locallab.spots.at(sp).loc.at(1) / 2000.0;
                    locyT[sp] = params->locallab.spots.at(sp).loc.at(3) / 2000.0;
                    centx[sp] = params->locallab.spots.at(sp).centerX / 2000.0 + 0.5;
                    centy[sp] = params->locallab.spots.at(sp).centerY / 2000.0 + 0.5;
                    blackredu[sp] = (params->locallab.spots.at(sp).smoothciemet == "gamnorol") || (params->locallab.spots.at(sp).smoothciemet == "gam") || (params->locallab.spots.at(sp).smoothciemet == "level") || (params->locallab.spots.at(sp).smoothciemet == "sigm") || (params->locallab.spots.at(sp).sigq12) || (params->locallab.spots.at(sp).sigjz12);

                   // const bool fullimstd = params->locallab.spots.at(sp).fullimage;//for log encoding standard
                   // const bool fullimjz = true;//always force fullimage in log encoding Jz - always possible to put a checkbox if need

                    //to auto sharp Capture
                    autoradius[sp] = params->locallab.spots.at(sp).deconvAutoRadius;
                    caprad[sp] = params->locallab.spots.at(sp).capradius;
                    float kradreduc = 1.f;
                    if(params->pdsharpening.enabled) {// reduce value if Capture Sharpening RAW used
                        kradreduc = 0.9f;
                    }
                    if (shar[sp] && autoradius[sp]){//calculate auto radius deconvolution.
                        float rad = -1.f;
                        if (imgsrc->getDeconvAutoRadius_capturesharpening_SE(&rad)) {//To call Capture Sharpening from Selective Editing
                            caprad[sp] = kradreduc * rad;
                        } else {
                            rad = -1.f;
                        }
                       
                    }
                        params->locallab.spots.at(sp).capradius = caprad[sp];
                        params->locallab.spots.at(sp).deconvAutoRadius = autoradius[sp];

 
                        LocallabListener::locallabsharBEF locsharbef;
                        locsharbef.capradiusbef = caprad[sp];
                        locsharbef.autoradiusbef = autoradius[sp];
                        locallsharbef.push_back(locsharbef);
 
                        if (locallListener) {
                            locallListener->sharbefChanged(locallsharbef,params->locallab.selspot); 
                        }
                        
                    
                    //end sharp capture

                    {//begin calculation blackev, whiteev, sourceg, sourceab
                        constexpr int SCALE = 10;
                        int fw, fh, tr = TR_NONE;
                        imgsrc->getFullSize(fw, fh, tr);
                        PreviewProps pp(0, 0, fw, fh, SCALE);
                        const bool fullimstd = params->locallab.spots.at(sp).fullimage;//for log encoding standard
                        const bool fullimjz = true;//always force fullimage in log encoding Jz - always possible to put a checkbox if need

                        float ysta = std::max(static_cast<float>(centy[sp] - locyT[sp]), 0.f);
                        float yend = std::min(static_cast<float>(centy[sp] + locy[sp]), 1.f);
                        float xsta = std::max(static_cast<float>(centx[sp] - locxL[sp]), 0.f);
                        float xend = std::min(static_cast<float>(centx[sp] + locx[sp]), 1.f);

                        if (fullimstd  && (log[sp] && autocomput[sp])) {
                            ysta = 0.f;
                            yend = 1.f;
                            xsta = 0.f;
                            xend = 1.f;
                        }

                        if (fullimjz  && (cie[sp] && autocie[sp])) {
                            ysta = 0.f;
                            yend = 1.f;
                            xsta = 0.f;
                            xend = 1.f;
                        }
                        if (log[sp] || cie[sp]) {
                            ipf.getAutoLogloc(sp, imgsrc, sourceg, blackev, whiteev, blackredu, Autogr, sourceab, whits, blacks, whitslog, blackslog, fw, fh, xsta, xend, ysta, yend, SCALE);
                        }
                        params->locallab.spots.at(sp).blackEv = blackev[sp];
                        params->locallab.spots.at(sp).whiteEv = whiteev[sp];
                        params->locallab.spots.at(sp).blackEvjz = blackev[sp];
                        params->locallab.spots.at(sp).whiteEvjz = whiteev[sp];
                        params->locallab.spots.at(sp).sourceGray = sourceg[sp];
                        params->locallab.spots.at(sp).sourceabs = sourceab[sp];
                        params->locallab.spots.at(sp).sourceGraycie = sourceg[sp];
                        params->locallab.spots.at(sp).sourceabscie = sourceab[sp];
                        params->locallab.spots.at(sp).whitescie = whits[sp];
                        params->locallab.spots.at(sp).blackscie = blacks[sp];
                        params->locallab.spots.at(sp).whiteslog = whitslog[sp];
                        params->locallab.spots.at(sp).blackslog = blackslog[sp];
                        float jz1 = defSpot.jz100;
 
                        LocallabListener::locallabcieBEF locciebef;
                        locciebef.blackevbef = blackev[sp];
                        locciebef.whiteevbef = whiteev[sp];
                        locciebef.sourcegbef = sourceg[sp];
                        locciebef.sourceabbef = sourceab[sp];
                        locciebef.targetgbef = targetg[sp];
                        locciebef.autocomputbef = autocomput[sp];
                        locciebef.autociebef = autocie[sp];
                        locciebef.jz1bef = jz1;
                        locallciebef.push_back(locciebef);

                        if (locallListener) {
                            locallListener->ciebefChanged(locallciebef,params->locallab.selspot); 
                        }
                    }
                }

                delete [] locx;
                delete [] locy;
                delete [] locxL;
                delete [] locyT;
                delete [] centx;
                delete [] centy;

                delete [] autocie;
                delete [] Autogr;
                delete [] whiteev;
                delete [] blackev;
                delete [] targetg;
                delete [] sourceab;
                delete [] whits;
                delete [] blacks;
                delete [] whitslog;
                delete [] blackslog;
                delete [] blackredu;
                delete [] sourceg;
                delete [] cie;
                delete [] log;
                delete [] shar;
                delete [] autocomput;
                delete [] autoradius;
                delete [] caprad;
            }
        }


        if ((todo & (M_AUTOEXP | M_RGBCURVE | M_CROP)) && params->locallab.enabled && !params->locallab.spots.empty()) {

            ipf.rgb2lab(*oprevi, *oprevl, params->icm.workingProfile);

            nprevl->CopyFrom(oprevl);
            //  int maxspot = 1;
            //*************************************************************
            // locallab
            //*************************************************************

            /*
             *  This file is part of RawTherapee.
             *
             *  Copyright (c) 2004-2010 Gabor Horvath <hgabor@rawtherapee.com>
             *
             *  RawTherapee is free software: you can redistribute it and/or modify
             *  it under the terms of the GNU General Public License as published by
             *  the Free Software Foundation, either version 3 of the License, or
             *  (at your option) any later version.
             *
             *  RawTherapee is distributed in the hope that it will be useful,
             *  but WITHOUT ANY WARRANTY; without even the implied warranty of
             *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
             *  GNU General Public License for more details.
             *
             *  You should have received a copy of the GNU General Public License
             *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
             *  2017 2018 Jacques Desmis <jdesmis@gmail.com>
             *  2019 Pierre Cabrera <pierre.cab@gmail.com>
             */
            const std::unique_ptr<LabImage> reserv(new LabImage(*oprevl, true));
            const std::unique_ptr<LabImage> lastorigimp(new LabImage(*oprevl, true));
            std::unique_ptr<LabImage> savenormdr;
            std::unique_ptr<LabImage> savenormtm;
            std::unique_ptr<LabImage> savenormreti;
            float **shbuffer = nullptr;
            int sca = 1;
            double huere, chromare, lumare, huerefblu, chromarefblu, lumarefblu, sobelre;
            float avge, meantme, stdtme, meanretie, stdretie;
            //std::vector<LocallabListener::locallabRef> locallref;
            std::vector<LocallabListener::locallabRetiMinMax> locallretiminmax;
            std::vector<LocallabListener::locallabcieLC> locallcielc;
            std::vector<LocallabListener::locallabshGHSbw> locallshgshbw;
            std::vector<LocallabListener::locallabshGHSbw2> locallshgshbw2;
            std::vector<LocallabListener::locallabsetLC> locallsetlc;
            std::vector<LocallabListener::locallabcieSIG> locallciesig;
            std::vector<LocallabListener::locallabshMICHbw> locallshmichbw;
            std::vector<LocallabListener::locallabcieFIN> locallciefin;

            huerefs.resize(params->locallab.spots.size());
            huerefblurs.resize(params->locallab.spots.size());
            chromarefblurs.resize(params->locallab.spots.size());
            lumarefblurs.resize(params->locallab.spots.size());
            chromarefs.resize(params->locallab.spots.size());
            lumarefs.resize(params->locallab.spots.size());
            sobelrefs.resize(params->locallab.spots.size());
            avgs.resize(params->locallab.spots.size());
            meantms.resize(params->locallab.spots.size());
            stdtms.resize(params->locallab.spots.size());
            meanretis.resize(params->locallab.spots.size());
            stdretis.resize(params->locallab.spots.size());
            const int sizespot = (int)params->locallab.spots.size();

            float *huerefp = nullptr;
            huerefp = new float[sizespot];
            float *chromarefp = nullptr;
            chromarefp = new float[sizespot];
            float *lumarefp = nullptr;
            lumarefp = new float[sizespot];
            float *fabrefp = nullptr;
            fabrefp = new float[sizespot];
            //new controls mainfp and scopefp with multi spots
            int *mainfp = nullptr;
            mainfp = new int[sizespot];
            int *scopefp = nullptr;
            scopefp = new int[sizespot];
            
            bool *autocontrast = nullptr;
            autocontrast = new bool[sizespot];

            for (int sp = 0; sp < (int)params->locallab.spots.size(); sp++) {

                if (params->locallab.spots.at(sp).equiltm  && params->locallab.spots.at(sp).exptonemap) {
                    savenormtm.reset(new LabImage(*oprevl, true));
                }

                if (params->locallab.spots.at(sp).equilret  && params->locallab.spots.at(sp).expreti) {
                    savenormreti.reset(new LabImage(*oprevl, true));
                }
                
               // if(params->locallab.spots.at(sp).colorscope != 30) {//compatibility with old method in controlspotpanel to change scope - default value 30
               //     scopefp[sp]= params->locallab.spots.at(sp).colorscope;
               // }
                
                // Set local curves of current spot to LUT
                locRETgainCurve.Set(params->locallab.spots.at(sp).localTgaincurve);
                locRETtransCurve.Set(params->locallab.spots.at(sp).localTtranscurve);
                const bool LHutili = loclhCurve.Set(params->locallab.spots.at(sp).LHcurve);
                const bool HHutili = lochhCurve.Set(params->locallab.spots.at(sp).HHcurve);
                const bool CHutili = locchCurve.Set(params->locallab.spots.at(sp).CHcurve);
                const bool HHutilijz = lochhCurvejz.Set(params->locallab.spots.at(sp).HHcurvejz);
                const bool CHutilijz = locchCurvejz.Set(params->locallab.spots.at(sp).CHcurvejz);
                const bool LHutilijz = loclhCurvejz.Set(params->locallab.spots.at(sp).LHcurvejz);
                const bool lcmasutili = locccmasCurve.Set(params->locallab.spots.at(sp).CCmaskcurve);
                const bool llmasutili = locllmasCurve.Set(params->locallab.spots.at(sp).LLmaskcurve);
                const bool lhmasutili = lochhmasCurve.Set(params->locallab.spots.at(sp).HHmaskcurve);
                const bool lhhmasutili = lochhhmasCurve.Set(params->locallab.spots.at(sp).HHhmaskcurve);
                const bool lhhmascieutili = lochhhmascieCurve.Set(params->locallab.spots.at(sp).HHhmaskciecurve);
                const bool llmasexputili = locllmasexpCurve.Set(params->locallab.spots.at(sp).LLmaskexpcurve);
                const bool lcmasexputili = locccmasexpCurve.Set(params->locallab.spots.at(sp).CCmaskexpcurve);
                const bool lhmasexputili = lochhmasexpCurve.Set(params->locallab.spots.at(sp).HHmaskexpcurve);
                const bool llmasSHutili = locllmasSHCurve.Set(params->locallab.spots.at(sp).LLmaskSHcurve);
                const bool lcmasSHutili = locccmasSHCurve.Set(params->locallab.spots.at(sp).CCmaskSHcurve);
                const bool lhmasSHutili = lochhmasSHCurve.Set(params->locallab.spots.at(sp).HHmaskSHcurve);
                const bool llmasvibutili = locllmasvibCurve.Set(params->locallab.spots.at(sp).LLmaskvibcurve);
                const bool lcmasvibutili = locccmasvibCurve.Set(params->locallab.spots.at(sp).CCmaskvibcurve);
                const bool lhmasvibutili = lochhmasvibCurve.Set(params->locallab.spots.at(sp).HHmaskvibcurve);
                const bool llmascbutili = locllmascbCurve.Set(params->locallab.spots.at(sp).LLmaskcbcurve);
                const bool lcmascbutili = locccmascbCurve.Set(params->locallab.spots.at(sp).CCmaskcbcurve);
                const bool lhmascbutili = lochhmascbCurve.Set(params->locallab.spots.at(sp).HHmaskcbcurve);
                const bool llmaslcutili = locllmaslcCurve.Set(params->locallab.spots.at(sp).LLmasklccurve);
                const bool lcmaslcutili = locccmaslcCurve.Set(params->locallab.spots.at(sp).CCmasklccurve);
                const bool lhmaslcutili = lochhmaslcCurve.Set(params->locallab.spots.at(sp).HHmasklccurve);
                const bool llmasretiutili = locllmasretiCurve.Set(params->locallab.spots.at(sp).LLmaskreticurve);
                const bool lcmasretiutili = locccmasretiCurve.Set(params->locallab.spots.at(sp).CCmaskreticurve);
                const bool lhmasretiutili = lochhmasretiCurve.Set(params->locallab.spots.at(sp).HHmaskreticurve);
                const bool llmastmutili = locllmastmCurve.Set(params->locallab.spots.at(sp).LLmasktmcurve);
                const bool lcmastmutili = locccmastmCurve.Set(params->locallab.spots.at(sp).CCmasktmcurve);
                const bool lhmastmutili = lochhmastmCurve.Set(params->locallab.spots.at(sp).HHmasktmcurve);
                const bool llmasblutili = locllmasblCurve.Set(params->locallab.spots.at(sp).LLmaskblcurve);
                const bool lcmasblutili = locccmasblCurve.Set(params->locallab.spots.at(sp).CCmaskblcurve);
                const bool lhmasblutili = lochhmasblCurve.Set(params->locallab.spots.at(sp).HHmaskblcurve);
                const bool llmaslogutili = locllmaslogCurve.Set(params->locallab.spots.at(sp).LLmaskcurveL);
                const bool lcmaslogutili = locccmaslogCurve.Set(params->locallab.spots.at(sp).CCmaskcurveL);
                const bool lhmaslogutili = lochhmaslogCurve.Set(params->locallab.spots.at(sp).HHmaskcurveL);
                const bool llmascieutili = locllmascieCurve.Set(params->locallab.spots.at(sp).LLmaskciecurve);
                const bool lcmascieutili = locccmascieCurve.Set(params->locallab.spots.at(sp).CCmaskciecurve);
                const bool lhmascieutili = lochhmascieCurve.Set(params->locallab.spots.at(sp).HHmaskciecurve);
                const bool lcmas_utili = locccmas_Curve.Set(params->locallab.spots.at(sp).CCmask_curve);
                const bool llmas_utili = locllmas_Curve.Set(params->locallab.spots.at(sp).LLmask_curve);
                const bool lhmas_utili = lochhmas_Curve.Set(params->locallab.spots.at(sp).HHmask_curve);
                const bool lhhmas_utili = lochhhmas_Curve.Set(params->locallab.spots.at(sp).HHhmask_curve);
                const bool lmasutiliblwav = loclmasCurveblwav.Set(params->locallab.spots.at(sp).LLmaskblcurvewav);
                const bool lmasutilicolwav = loclmasCurvecolwav.Set(params->locallab.spots.at(sp).LLmaskcolcurvewav);
                const bool lmasutiliciewav = loclmasCurveciewav.Set(params->locallab.spots.at(sp).LLmaskciecurvewav);
                const bool locwavutili = locwavCurve.Set(params->locallab.spots.at(sp).locwavcurve);
                const bool locwavutilijz = locwavCurvejz.Set(params->locallab.spots.at(sp).locwavcurvejz);
                const bool loclevwavutili = loclevwavCurve.Set(params->locallab.spots.at(sp).loclevwavcurve);
                const bool locconwavutili = locconwavCurve.Set(params->locallab.spots.at(sp).locconwavcurve);
                const bool loccompwavutili = loccompwavCurve.Set(params->locallab.spots.at(sp).loccompwavcurve);
                const bool loccomprewavutili = loccomprewavCurve.Set(params->locallab.spots.at(sp).loccomprewavcurve);
                const bool locwavhueutili = locwavCurvehue.Set(params->locallab.spots.at(sp).locwavcurvehue);
                const bool locwavhueutilicont = locwavCurvehuecont.Set(params->locallab.spots.at(sp).locwavcurvehuecont);
                const bool locwavdenutili = locwavCurveden.Set(params->locallab.spots.at(sp).locwavcurveden);
                const bool locedgwavutili = locedgwavCurve.Set(params->locallab.spots.at(sp).locedgwavcurve);
                const bool lmasutili_wav = loclmasCurve_wav.Set(params->locallab.spots.at(sp).LLmask_curvewav);
                const bool locallutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).llcurve, lllocalcurve, sca);
                const bool localclutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).clcurve, cllocalcurve, sca);
                const bool locallcutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).lccurve, lclocalcurve, sca);
                const bool localcutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).cccurve, cclocalcurve, sca);
                const bool localrgbutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).rgbcurve, rgblocalcurve, sca);
                const bool localexutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).excurve, exlocalcurve, sca);
                const bool localmaskutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskcurve, lmasklocalcurve, sca);
                const bool localmaskexputili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskexpcurve, lmaskexplocalcurve, sca);
                const bool localmaskSHutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).LmaskSHcurve, lmaskSHlocalcurve, sca);
              //  const bool localghsutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).ghscurve, ghslocalcurve, sca);
                const bool localmaskvibutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskvibcurve, lmaskviblocalcurve, sca);
                const bool localmasktmutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmasktmcurve, lmasktmlocalcurve, sca);
                const bool localmaskretiutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskreticurve, lmaskretilocalcurve, sca);
                const bool localmaskcbutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskcbcurve, lmaskcblocalcurve, sca);
                const bool localmaskblutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskblcurve, lmaskbllocalcurve, sca);
                const bool localmasklcutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmasklccurve, lmasklclocalcurve, sca);
                const bool localmasklogutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).LmaskcurveL, lmaskloglocalcurve, sca);
                const bool localmask_utili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmask_curve, lmasklocal_curve, sca);
                const bool localmaskcieutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).Lmaskciecurve, lmaskcielocalcurve, sca);
                const bool localcieutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).ciecurve, cielocalcurve, sca);
                const bool localcieutili2 = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).ciecurve2, cielocalcurve2, sca);
                const bool localjzutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).jzcurve, jzlocalcurve, sca);
                const bool localczutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).czcurve, czlocalcurve, sca);
                const bool localczjzutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).czjzcurve, czjzlocalcurve, sca);
                const bool localredutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).redcurve, redlocalcurve, sca);
                const bool localgreenutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).greencurve, greenlocalcurve, sca);
                const bool localblueutili = CurveFactory::diagonalCurve2Lut(params->locallab.spots.at(sp).bluecurve, bluelocalcurve, sca);

                double ecomp = params->locallab.spots.at(sp).expcomp;
                double black = params->locallab.spots.at(sp).black;
                double hlcompr = params->locallab.spots.at(sp).hlcompr;
                double hlcomprthresh = params->locallab.spots.at(sp).hlcomprthresh;
                double shcompr = params->locallab.spots.at(sp).shcompr;
                double br = params->locallab.spots.at(sp).lightness;
                double cont = params->locallab.spots.at(sp).contrast;
                float contsig = params->locallab.spots.at(sp).contsigqcie;
            
                float lightsig = params->locallab.spots.at(sp).lightsigqcie;
          
                if (black < 0. && params->locallab.spots.at(sp).expMethod == "pde") {
                    black *= 1.5;
                }

                // Reference parameters computation
                if (params->locallab.spots.at(sp).spotMethod == "exc") {
                    ipf.calc_ref(sp, reserv.get(), reserv.get(), 0, 0, pW, pH, scale, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                } else {
                    ipf.calc_ref(sp, nprevl, nprevl, 0, 0, pW, pH, scale, huerefblu, chromarefblu, lumarefblu, huere, chromare, lumare, sobelre, avge, locwavCurveden, locwavdenutili);
                }

                meantme = 0.f;
                stdtme = 0.f;
                meanretie = 0.f;
                stdretie = 0.f;
                float fab = 1.f;
                float maxicam = -1000.f;
                float rdx, rdy, grx, gry, blx, bly = 0.f;
                float meanx, meany, meanxe, meanye, maxdat = 0.f;
                int ill = 2;
                int prim = 3;
                bool istm = params->locallab.spots.at(sp).equiltm  && params->locallab.spots.at(sp).exptonemap;
                bool isreti = params->locallab.spots.at(sp).equilret  && params->locallab.spots.at(sp).expreti;
                //preparation for mean and sigma on current RT-spot
                float locx = 0.f;
                float locy = 0.f;
                float locxl = 0.f;
                float locyt = 0.f;
                float centx = 0.f;
                float centy = 0.f;
                float ysta = 0.f;
                float yend = 1.f;
                float xsta = 0.f;
                float xend = 1.f;

                if (istm || isreti) {
                    locx = params->locallab.spots.at(sp).loc.at(0) / 2000.0;
                    locy = params->locallab.spots.at(sp).loc.at(2) / 2000.0;
                    locxl = params->locallab.spots.at(sp).loc.at(1) / 2000.0;
                    locyt = params->locallab.spots.at(sp).loc.at(3) / 2000.0;
                    centx = params->locallab.spots.at(sp).centerX / 2000.0 + 0.5;
                    centy = params->locallab.spots.at(sp).centerY / 2000.0 + 0.5;
                    ysta = std::max(static_cast<float>(centy - locyt), 0.f);
                    yend = std::min(static_cast<float>(centy + locy), 1.f);
                    xsta = std::max(static_cast<float>(centx - locxl), 0.f);
                    xend = std::min(static_cast<float>(centx + locx), 1.f);
                }

                int ww = nprevl->W;
                int hh = nprevl->H;
                int xxs = xsta * ww;
                int xxe = xend * ww;
                int yys = ysta * hh;
                int yye = yend * hh;

                if (istm) { //calculate mean and sigma on full image for RT-spot use by normalize_mean_dt
                    ipf.mean_sig(nprevl->L, meantme, stdtme, xxs, xxe, yys, yye);
                }

                if (isreti) { //calculate mean and sigma on full image for RT-spot use by normalize_mean_dt
                    ipf.mean_sig(nprevl->L, meanretie, stdretie, xxs, xxe, yys, yye) ;
                }

                double huerblu = huerefblurs[sp] = huerefblu;
                double chromarblu = chromarefblurs[sp] = chromarefblu;
                double lumarblu = lumarefblurs[sp] = lumarefblu;
                double huer = huerefs[sp] = huere;
                double chromar = chromarefs[sp] = chromare;
                double lumar = lumarefs[sp] = lumare ;
                double sobeler = sobelrefs[sp] = sobelre;
                float avg = avgs[sp] = avge;
                float meantm = meantms[sp] = meantme;
                float stdtm = stdtms[sp] = stdtme;
                float meanreti = meanretis[sp] = meanretie;
                float stdreti = stdretis[sp] = stdretie;
                huerefp[sp] = huer;
                chromarefp[sp] = chromar;
                lumarefp[sp] = lumar;
                
                autocontrast[sp] = params->locallab.spots.at(sp).deconvAutoshar;


                CurveFactory::complexCurvelocal(ecomp, black / 65535., hlcompr, hlcomprthresh, shcompr, br, cont, lumar,
                                                hltonecurveloc, shtonecurveloc, tonecurveloc, lightCurveloc, avg,
                                                sca);

                // Save Locallab mask curve references for current spot
                // Locallab tools computation
                /* Notes:
                 * - shbuffer is used as nullptr
                 */

                // Locallab mask is only showed in detailed image
                float minCD;
                float maxCD;
                float mini;
                float maxi;
                float Tmean;
                float Tsigma;
                float Tmin;
                float Tmax;
                int lastsav;

                float resi[8];             
                float sharc = 0.f;
                float denocont = 0.f;
                int ghsbpwp[2] = {0, 0};
                float ghsbpwpvalue[2] = {0.f, 1.f};                
                float savmadl[21];//just to intialize, not used here - but in dcrop.cc
                float ghsbwslider[2] = {0.f, 1.f};// Black and white point auto sliders
                float ghssym = 0.f;//info symmetry point
                float ghsmid = 0.f;//info estimated men luminance (Mid grey) %
                float ghsmaxrgb = 0.f;//info maximum RGB data after GHS
                float ghs3sig = 0.f;//Info 3 standard deviations
                float ghscolor[4] = {0.f, 0.f, 0.f, 0.f};
                bool ghsauto = params->locallab.spots.at(sp).ghs_autobw;
                bool ghsautsp = false;//SP auto
                float michbwslider[2] = {0.f, 1.f};// Black and white point auto sliders : added to facilitate a possible modification requested by users, but is not currently in use
                float maxdatend2 = 0.f;
                float satdatend2 = 0.f;
                bool gamaut2 = false;
                Glib::ustring prof = params->icm.workingProfile;
                if(params->locallab.spots.at(sp).complexcie == 2) {
                    params->locallab.spots.at(sp).primMethod = prof;//in Basic mode set to Working profile
                }
                float slopeg = 1.f;
                bool linkrgb = true;

                ipf.Lab_Local(3, sp, (float**)shbuffer, nprevl, nprevl, reserv.get(), savenormtm.get(), savenormreti.get(), lastorigimp.get(), fw, fh, 0, 0, pW, pH, pW, pH, pW, pH,  scale, locRETgainCurve, locRETtransCurve,
                              lllocalcurve, locallutili,
                              cllocalcurve, localclutili,
                              lclocalcurve, locallcutili,
                              loclhCurve,  lochhCurve, locchCurve,
                              lochhCurvejz, locchCurvejz, loclhCurvejz,
                              lmasklocalcurve, localmaskutili,
                              lmaskexplocalcurve, localmaskexputili,
                              lmaskSHlocalcurve, localmaskSHutili,
                              lmaskviblocalcurve, localmaskvibutili,
                              lmasktmlocalcurve, localmasktmutili,
                              lmaskretilocalcurve, localmaskretiutili,
                              lmaskcblocalcurve, localmaskcbutili,
                              lmaskbllocalcurve, localmaskblutili,
                              lmasklclocalcurve, localmasklcutili,
                              lmaskloglocalcurve, localmasklogutili,
                              lmasklocal_curve, localmask_utili,
                              lmaskcielocalcurve, localmaskcieutili,
                              cielocalcurve, localcieutili,
                              cielocalcurve2, localcieutili2,
                              jzlocalcurve, localjzutili,
                              czlocalcurve, localczutili,
                              czjzlocalcurve, localczjzutili,
                              redlocalcurve, localredutili,
                              greenlocalcurve, localgreenutili,
                              bluelocalcurve, localblueutili,
                              locccmasCurve, lcmasutili, locllmasCurve, llmasutili, lochhmasCurve, lhmasutili, lochhhmasCurve, lhhmasutili, lochhhmascieCurve, lhhmascieutili, locccmasexpCurve, lcmasexputili, locllmasexpCurve, llmasexputili, lochhmasexpCurve, lhmasexputili,
                              locccmasSHCurve, lcmasSHutili, locllmasSHCurve, llmasSHutili, lochhmasSHCurve, lhmasSHutili,
                              locccmasvibCurve, lcmasvibutili, locllmasvibCurve, llmasvibutili, lochhmasvibCurve, lhmasvibutili,
                              locccmascbCurve, lcmascbutili, locllmascbCurve, llmascbutili, lochhmascbCurve, lhmascbutili,
                              locccmasretiCurve, lcmasretiutili, locllmasretiCurve, llmasretiutili, lochhmasretiCurve, lhmasretiutili,
                              locccmastmCurve, lcmastmutili, locllmastmCurve, llmastmutili, lochhmastmCurve, lhmastmutili,
                              locccmasblCurve, lcmasblutili, locllmasblCurve, llmasblutili, lochhmasblCurve, lhmasblutili,
                              locccmaslcCurve, lcmaslcutili, locllmaslcCurve, llmaslcutili, lochhmaslcCurve, lhmaslcutili,
                              locccmaslogCurve, lcmaslogutili, locllmaslogCurve, llmaslogutili, lochhmaslogCurve, lhmaslogutili,

                              locccmas_Curve, lcmas_utili, locllmas_Curve, llmas_utili, lochhmas_Curve, lhmas_utili,
                              locccmascieCurve, lcmascieutili, locllmascieCurve, llmascieutili, lochhmascieCurve, lhmascieutili,

                              lochhhmas_Curve, lhhmas_utili,
                              loclmasCurveblwav, lmasutiliblwav,
                              loclmasCurvecolwav, lmasutilicolwav,
                              loclmasCurveciewav, lmasutiliciewav,
                              locwavCurve, locwavutili,
                              locwavCurvejz, locwavutilijz,
                              loclevwavCurve, loclevwavutili,
                              locconwavCurve, locconwavutili,
                              loccompwavCurve, loccompwavutili,
                              loccomprewavCurve, loccomprewavutili,
                              locwavCurvehue, locwavhueutili,
                              locwavCurvehuecont, locwavhueutilicont,
                              locwavCurveden, locwavdenutili,
                              locedgwavCurve, locedgwavutili,
                              loclmasCurve_wav, lmasutili_wav,
                              LHutili, HHutili, CHutili, HHutilijz, CHutilijz, LHutilijz, cclocalcurve, localcutili, rgblocalcurve, localrgbutili, localexutili, exlocalcurve, hltonecurveloc, shtonecurveloc, tonecurveloc, lightCurveloc,
                              huerblu, chromarblu, lumarblu, huer, chromar, lumar, sobeler, lastsav, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax,
                              meantm, stdtm, meanreti, stdreti, fab, maxicam, rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, prim, ill, contsig, lightsig, slopeg, linkrgb,
                              resi, sharc, denocont, ghsbpwp, ghsbpwpvalue, savmadl, ghsbwslider, ghssym, ghsautsp, ghscolor, ghsmid, ghsmaxrgb, ghs3sig, michbwslider, maxdatend2, satdatend2, gamaut2);

                fabrefp[sp] = fab;
                //Illuminant
                float w_x = 0.3f;
                float w_y = 0.3f;
                if(ill == 2) {
                    w_x = 0.3457f;
                    w_y = 0.3585f;
                } else if(ill == 4) {
                    w_x = 0.3217f;
                    w_y = 0.3377f;
                } else if(ill == 5) {
                    w_x = 0.3127f;
                    w_y = 0.3290f;
                } else if(ill == 1) {
                    w_x = 0.376137f;
                    w_y = 0.374021f;
                } else if(ill == 3) {
                    w_x = 0.332424f;
                    w_y = 0.347426f;
                } else if(ill == 6) {
                    w_x = 0.293756f;
                    w_y = 0.309185f;
                } else if(ill == 7) {//D120
                    w_x = 0.269669f;                 
                    w_y = 0.28078f;
                } else if(ill == 8) {//stdA
                    w_x = 0.447573f;
                    w_y = 0.407440f;
                } else if(ill == 9) {//2000K
                    w_x = 0.526591f;
                    w_y = 0.41331f;
                } else if(ill == 10) {//1500K
                    w_x = 0.585703f;
                    w_y = 0.393157f;
                } else if(ill == 20) {
                    w_x = 0.333333f;
                    w_y = 0.333333f;
                }
                //move white-point in GUI
                double refin = params->locallab.spots.at(sp).refi;
                double arefi = (w_y - meany) / (w_x - meanx);
                double brefi = w_y - arefi * w_x;
                double scalrefi = meanx - w_x;
                w_x = w_x + scalrefi * refin;
                w_y = w_x * arefi + brefi;
              
      
      
                if (istm) { //calculate mean and sigma on full image for use by normalize_mean_dt
                    float meanf = 0.f;
                    float stdf = 0.f;
                    ipf.mean_sig(savenormtm->L, meanf, stdf, xxs, xxe, yys, yye);

                    //using 2 unused variables  noiselumc and softradiustm
                    params->locallab.spots.at(sp).noiselumc = (int) meanf;
                    params->locallab.spots.at(sp).softradiustm = stdf ;
                }

                if (isreti) { //calculate mean and sigma on full image for use by normalize_mean_dt
                    float meanf = 0.f;
                    float stdf = 0.f;
                    ipf.mean_sig(savenormreti->L, meanf, stdf, xxs, xxe, yys, yye);
                    //using 2 unused variables  sensihs and sensiv
                    params->locallab.spots.at(sp).sensihs = (int) meanf;
                    params->locallab.spots.at(sp).sensiv = (int) stdf;
                }


                if (sp + 1u < params->locallab.spots.size()) {
                    // do not copy for last spot as it is not needed anymore
                    lastorigimp->CopyFrom(nprevl);
                }

                // Save Locallab Retinex min/max for current spot
                LocallabListener::locallabRetiMinMax retiMinMax;
                retiMinMax.cdma = maxCD;
                retiMinMax.cdmin = minCD;
                retiMinMax.mini = mini;
                retiMinMax.maxi = maxi;
                retiMinMax.Tmean = Tmean;
                retiMinMax.Tsigma = Tsigma;
                retiMinMax.Tmin = Tmin;
                retiMinMax.Tmax = Tmax;
                locallretiminmax.push_back(retiMinMax);

                //save Locallab CIE primaries and white for current spot
                LocallabListener::locallabcieLC loccielc;
                loccielc.redxlc = rdx;
                loccielc.redylc = rdy;
                loccielc.grexlc = grx;
                loccielc.greylc = gry;
                loccielc.bluxlc = blx;
                loccielc.bluylc = bly;
                loccielc.wxlc = w_x;
                loccielc.wylc = w_y;
                loccielc.meanxlc = meanx;
                loccielc.meanylc = meany;
                loccielc.meanxelc = meanxe;
                loccielc.meanyelc = meanye;
                loccielc.slopeglc = slopeg;
                loccielc.linkrgblc = linkrgb;
                locallcielc.push_back(loccielc);

                LocallabListener::locallabcieSIG locciesig;
                locciesig.contsigq = contsig;
                locciesig.lightsigq = lightsig;
                locallciesig.push_back(locciesig);

                LocallabListener::locallabcieFIN locciefin;
                locciefin.max_rgb = maxdatend2;
                locciefin.max_sat = satdatend2;
                locciefin.gam_aut = gamaut2;
                locallciefin.push_back(locciefin);

                LocallabListener::locallabshGHSbw2 locshghsbw2;//ghs sliders Black and white point
                    for(int j = 0; j < 2; j++) {
                        locshghsbw2.ghsbw_slider[j] = ghsbwslider[j];
                    }
                    locshghsbw2.ghs_auto = ghsauto;
                locallshgshbw2.push_back(locshghsbw2);



                LocallabListener::locallabshGHSbw locshghsbw;//ghs Black and white point infos, SP, middle grey, max RGB
                    for(int j = 0; j < 2; j++) {
                        locshghsbw.ghsbw[j] = ghsbpwp[j];
                        locshghsbw.ghsbwvalue[j] = ghsbpwpvalue[j];
                    }
                    locshghsbw.ghs_sym = ghssym;
                    locshghsbw.ghs_mid = ghsmid;
                    locshghsbw.ghs_maxrgb = ghsmaxrgb;
                    locshghsbw.ghs_3sig = ghs3sig;
                    locshghsbw.ghs_color[0] = ghscolor[0];
                    locshghsbw.ghs_color[1] = ghscolor[1];
                    locshghsbw.ghs_color[2] = ghscolor[2];
                    locshghsbw.ghs_color[3] = ghscolor[3];
                    locshghsbw.autoSP =  ghsautsp;//SP auto
                    locshghsbw.ghs_auto = ghsauto;
                locallshgshbw.push_back(locshghsbw);

                const bool michblack = params->locallab.spots.at(sp).mich_black;//Linear Subtract Black point
                const bool michwhite = params->locallab.spots.at(sp).mich_white;//Linear White point
                bool calculatbw = false;
                calculatbw = michblack || michwhite;

                LocallabListener::locallabshMICHbw locshmich_bw;
                    for(int j = 0; j < 2; j++) {
                        locshmich_bw.mich_slider[j] = michbwslider[j];
                    }
                    locshmich_bw.mich_auto = calculatbw;

                locallshmichbw.push_back(locshmich_bw);

                // Recalculate references after
                if (params->locallab.spots.at(sp).spotMethod == "exc") {
                    ipf.calc_ref(sp, reserv.get(), reserv.get(), 0, 0, pW, pH, scale, huerefblu, chromarefblu, lumarefblu, huer, chromar, lumar, sobeler, avg, locwavCurveden, locwavdenutili);
                } else {
                    ipf.calc_ref(sp, nprevl, nprevl, 0, 0, pW, pH, scale, huerefblu, chromarefblu, lumarefblu, huer, chromar, lumar, sobeler, avg, locwavCurveden, locwavdenutili);
                }

                // Update Locallab reference values according to recurs parameter
                if (params->locallab.spots.at(sp).recurs) {
                    huerefp[sp] = huer;
                    chromarefp[sp] = chromar;
                    lumarefp[sp] = lumar;
                    fabrefp[sp] = fab;

                }
                
                // new used linked to global and scope 
                mainfp[sp] = 0;
                if (params->locallab.spots.at(sp).spotMethod == "main") {
                    mainfp[sp] = 3;
                } else if (params->locallab.spots.at(sp).spotMethod == "full") {
                    mainfp[sp] = 2;
                }
                //keep using tools
                bool iscolor = params->locallab.spots.at(sp).expcolor;
                bool issh = params->locallab.spots.at(sp).expshadhigh;
                bool isvib = params->locallab.spots.at(sp).expvibrance;
                bool isexpos = params->locallab.spots.at(sp).expexpose;
                bool issoft = params->locallab.spots.at(sp).expsoft;
                bool isblur = params->locallab.spots.at(sp).expblur;
                bool istom = params->locallab.spots.at(sp).exptonemap;
                bool isret = params->locallab.spots.at(sp).expreti;
                bool issharp = params->locallab.spots.at(sp).expsharp;
                bool iscont = params->locallab.spots.at(sp).expcontrast;
                bool iscbdl = params->locallab.spots.at(sp).expcbdl;
                bool islog = params->locallab.spots.at(sp).explog;
                bool ismas = params->locallab.spots.at(sp).expmask;
                bool isci = params->locallab.spots.at(sp).expcie;
              // bool isset = iscolor || issh || isvib;
                
                //set select spot settings 
                LocallabListener::locallabsetLC locsetlc;
                locsetlc.mainf = mainfp[sp];
                locsetlc.iscolo = iscolor;
                locsetlc.iss = issh;
                locsetlc.isvi = isvib;
                locsetlc.isexpo = isexpos;
                locsetlc.issof = issoft;
                locsetlc.isblu = isblur;
                locsetlc.isto = istom;
                locsetlc.isre = isret;
                locsetlc.isshar = issharp;
                locsetlc.iscon = iscont;
                locsetlc.iscbd = iscbdl;
                locsetlc.islo = islog;
                locsetlc.isma = ismas;
                locsetlc.isci = isci;
                locallsetlc.push_back(locsetlc);
                
                if (locallListener) {
                    locallListener->refChanged2(huerefp, chromarefp, lumarefp, fabrefp, params->locallab.selspot);
                    locallListener->minmaxChanged(locallretiminmax, params->locallab.selspot);
                    if (params->locallab.spots.at(sp).expprecam) {
                        locallListener->cieChanged(locallcielc,params->locallab.selspot); 
                    }

                    if (params->locallab.spots.at(sp).expcie) {
                        locallListener->sigChanged(locallciesig,params->locallab.selspot);
                        locallListener->finChanged(locallciefin,params->locallab.selspot);
                    }

                    if (params->locallab.spots.at(sp).expshadhigh && params->locallab.spots.at(sp).shMethod == "ghs") {
                        locallListener->ghsbw2Changed(locallshgshbw2,params->locallab.selspot);//Black and White point slider
                    }

                    if (params->locallab.spots.at(sp).expshadhigh && params->locallab.spots.at(sp).shMethod == "ghs") {
                        locallListener->ghsbwChanged(locallshgshbw,params->locallab.selspot);//Black and White point infos, SP auto, Middle grey, max RGB
                    }

                    if (params->locallab.spots.at(sp).expshadhigh && params->locallab.spots.at(sp).shMethod == "micha") {
                        locallListener->michbwChanged(locallshmichbw,params->locallab.selspot);//Subtract Black and White point infos - Michaelis
                    }

                    /*
                    if(params->locallab.spots.at(sp).colorscope != 0) {//compatibility with old method in controlspotpanel
                            locallListener->scopeChangedcol(scopefp[sp], params->locallab.selspot, iscolor);
                            locallListener->scopeChangedsh(scopefp[sp], params->locallab.selspot, issh);
                            locallListener->scopeChangedvib(scopefp[sp], params->locallab.selspot, isvib);
                            locallListener->scopeChangedset(scopefp[sp], params->locallab.selspot, isset);
                            //params->locallab.spots.at(sp).colorscope = 30;
                    }
                   */
                   // if (mainfp[sp] >= 0) {//minimize call to idle register 
                        //used by Global fullimage.
                    locallListener->maiChanged(locallsetlc,params->locallab.selspot); 
                   // }

                }
                
            }

            delete [] huerefp;
            delete [] chromarefp;
            delete [] lumarefp;
            delete [] fabrefp;
            delete [] mainfp;
            delete [] scopefp;
            delete [] autocontrast;
            
            ipf.lab2rgb(*nprevl, *oprevi, params->icm.workingProfile);
            //*************************************************************
            // end locallab
            //*************************************************************

        }
        if ((todo & M_RGBCURVE) || (todo & M_CROP)) {
            // complexCurve also calculates the pre-curves histogram depending
            // on crop; the tone/LUT part is shared with the viewport fast path.
            prepareViewportLuts(M_RGBCURVE);

            int satTH = 80;
            int satPR = 30;
            int indi = 0;

            if (params->colorToning.enabled  && params->colorToning.autosat && params->colorToning.method != "LabGrid") { //for colortoning evaluation of saturation settings
                float moyS = 0.f;
                float eqty = 0.f;
                ipf.moyeqt(oprevi, moyS, eqty); //return image : mean saturation and standard dev of saturation
                //printf("moy=%f ET=%f\n", moyS,eqty);
                float satp = ((moyS + 1.5f * eqty) - 0.3f) / 0.7f; //1.5 sigma ==> 93% pixels with high saturation -0.3 / 0.7 convert to Hombre scale

                if (satp >= 0.92f) {
                    satp = 0.92f;    //avoid values too high (out of gamut)
                }

                if (satp <= 0.15f) {
                    satp = 0.15f;    //avoid too low values
                }

                //satTH=(int) 100.f*satp;
                //satPR=(int) 100.f*(moyS-0.85f*eqty);//-0.85 sigma==>20% pixels with low saturation
                colourToningSatLimit = 100.f * satp;
                satTH = (int) 100.f * satp;

                colourToningSatLimitOpacity = 100.f * (moyS - 0.85f * eqty); //-0.85 sigma==>20% pixels with low saturation
                satPR = (int) 100.f * (moyS - 0.85f * eqty);
            }

            if (actListener && params->colorToning.enabled) {
                if (params->blackwhite.enabled && params->colorToning.autosat) {
                    actListener->autoColorTonChanged(0, satTH, satPR);    //hide sliders only if autosat
                    indi = 0;
                } else {
                    if (params->colorToning.autosat) {
                        if (params->colorToning.method == "Lab") {
                            indi = 1;
                        } else if (params->colorToning.method == "RGBCurves") {
                            indi = 1;
                        } else if (params->colorToning.method == "RGBSliders") {
                            indi = 1;
                        } else if (params->colorToning.method == "Splico") {
                            indi = 2;
                        } else if (params->colorToning.method == "Splitlr") {
                            indi = 2;
                        }
                    }
                }
            }

            // if it's just crop we just need the histogram, no image updates
            if (todo & M_RGBCURVE) {
                //initialize rrm bbm ggm different from zero to avoid black screen in some cases
                double rrm = 33.;
                double ggm = 33.;
                double bbm = 33.;

                DCPProfileApplyState as;
                DCPProfile *dcpProf = imgsrc->getDCP(params->icm, as);

                ipf.rgbProc(oprevi, oprevl, nullptr, hltonecurve, shtonecurve, tonecurve, params->toneCurve.saturation,
                            rCurve, gCurve, bCurve, colourToningSatLimit, colourToningSatLimitOpacity, ctColorCurve, ctOpacityCurve, opautili, clToningcurve, cl2Toningcurve, customToneCurve1, customToneCurve2, beforeToneCurveBW, afterToneCurveBW, rrm, ggm, bbm, bwAutoR, bwAutoG, bwAutoB, params->toneCurve.expcomp, params->toneCurve.hlcompr, params->toneCurve.hlcomprthresh, dcpProf, as, histToneCurve);

                if (params->blackwhite.enabled && params->blackwhite.autoc && abwListener) {
                    if (settings->verbose) {
                        printf("ImProcCoordinator / Auto B&W coefs:   R=%.2f   G=%.2f   B=%.2f\n", static_cast<double>(bwAutoR), static_cast<double>(bwAutoG), static_cast<double>(bwAutoB));
                    }

                    abwListener->BWChanged((float) rrm, (float) ggm, (float) bbm);
                }

                if (params->colorToning.enabled && params->colorToning.autosat && actListener) {
                    actListener->autoColorTonChanged(indi, (int) colourToningSatLimit, (int)colourToningSatLimitOpacity);  //change sliders autosat
                }

                // correct GUI black and white with value
            }

            //  ipf.Lab_Tile(oprevl, oprevl, scale);

            // compute L channel histogram
            int x1, y1, x2, y2;
            params->crop.mapToResized(pW, pH, scale, x1, x2,  y1, y2);
        }

//    lhist16(32768);
        if (todo & (M_LUMACURVE | M_CROP)) {
            LUTu lhist16(32768);
            lhist16.clear();
#ifdef _OPENMP
            const int numThreads = min(max(pW * pH / (int)lhist16.getSize(), 1), omp_get_max_threads());
            #pragma omp parallel num_threads(numThreads) if (numThreads>1)
#endif
            {
                LUTu lhist16thr(lhist16.getSize());
                lhist16thr.clear();
#ifdef _OPENMP
                #pragma omp for nowait
#endif

                for (int x = 0; x < pH; x++)
                    for (int y = 0; y < pW; y++) {
                        int pos = (int)(oprevl->L[x][y]);
                        lhist16thr[pos]++;
                    }

#ifdef _OPENMP
                #pragma omp critical
#endif
                lhist16 += lhist16thr;
            }
#ifdef _OPENMP
            static_cast<void>(numThreads);  // to silence cppcheck warning
#endif
            CurveFactory::complexLCurve(params->labCurve.brightness, params->labCurve.contrast, params->labCurve.lcurve, lhist16, lumacurve, histLCurve, scale == 1 ? 1 : 16, utili);
        }

        if (todo & M_LUMACURVE) {

            clcutili = CurveFactory::diagonalCurve2Lut(params->labCurve.clcurve, clcurve, scale == 1 ? 1 : 16);

            CurveFactory::complexsgnCurve(autili, butili, ccutili, cclutili, params->labCurve.acurve, params->labCurve.bcurve, params->labCurve.cccurve,
                                          params->labCurve.lccurve, chroma_acurve, chroma_bcurve, satcurve, lhskcurve, scale == 1 ? 1 : 16);
            updateViewportLutSnapshot(M_LUMACURVE);
        }

        //scale = 1;

        if ((todo & (M_LUMINANCE + M_COLOR)) || (todo & M_AUTOEXP)) {
            nprevl->CopyFrom(oprevl);
            histCCurve.clear();
            histLCurve.clear();

            if (params->colorToning.enabled && params->colorToning.method == "LabGrid") {
                ipf.colorToningLabGrid(nprevl, 0, nprevl->W, 0, nprevl->H, false);
            }

            ipf.shadowsHighlights(nprevl, params->sh.enabled, params->sh.lab, params->sh.highlights, params->sh.shadows, params->sh.radius, scale, params->sh.htonalwidth, params->sh.stonalwidth);

            if (params->localContrast.enabled) {
                // Alberto's local contrast
                ipf.localContrast(nprevl, nprevl->L, params->localContrast, false, scale);
            }

            ipf.chromiLuminanceCurve(nullptr, pW, nprevl, nprevl, chroma_acurve, chroma_bcurve, satcurve, lhskcurve, clcurve, lumacurve, utili, autili, butili, ccutili, cclutili, clcutili, histCCurve, histLCurve);
            ipf.vibrance(nprevl, params->vibrance, params->toneCurve.hrenabled, params->icm.workingProfile);
            ipf.labColorCorrectionRegions(nprevl);

         //   if ((params->colorappearance.enabled && !params->colorappearance.tonecie) || (!params->colorappearance.enabled)) {
            if ((params->colorappearance.enabled && !params->colorappearance.tonecie) || (!cam02)) {
                ipf.EPDToneMap(nprevl, 0, scale);
            }

            if (params->dirpyrequalizer.cbdlMethod == "aft") {
              //  if (((params->colorappearance.enabled && !settings->autocielab) || (!params->colorappearance.enabled))) {
                if (((params->colorappearance.enabled && !settings->autocielab) || (!cam02))) {
                    ipf.dirpyrequalizer(nprevl, scale);
                }
            }

            wavcontlutili = CurveFactory::diagonalCurve2Lut(params->wavelet.wavclCurve, wavclCurve, scale == 1 ? 1 : 16);
            updateViewportLutSnapshot(M_LUMINANCE | M_COLOR);

            if ((params->wavelet.enabled)) {
                WaveletParams WaveParams = params->wavelet;
                WaveParams.getCurves(wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL);
                int kall = 0;
                LabImage *unshar = nullptr;
                Glib::ustring provis;
                LabImage *provradius = nullptr;
                bool procont = WaveParams.expcontrast;
                bool prochro = WaveParams.expchroma;
                bool proedge = WaveParams.expedge;
                bool profin = WaveParams.expfinal;
                bool proton = WaveParams.exptoning;
                bool pronois = WaveParams.expnoise;

                if (WaveParams.showmask) {
                    //   WaveParams.showmask = false;
                    //   WaveParams.expclari = true;
                }

                if (WaveParams.softrad > 0.f) {
                    provradius = new LabImage(*nprevl, true);
                }

                if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
                    provis = params->wavelet.CLmethod;
                    params->wavelet.CLmethod = "all";
                    ipf.ip_wavelet(nprevl, nprevl, kall, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, wavclCurve, scale);
                    unshar = new LabImage(*nprevl, true);

                    params->wavelet.CLmethod = provis;

                    WaveParams.expcontrast = false;
                    WaveParams.expchroma = false;
                    WaveParams.expedge = false;
                    WaveParams.expfinal = false;
                    WaveParams.exptoning = false;
                    WaveParams.expnoise = false;
                }

                ipf.ip_wavelet(nprevl, nprevl, kall, WaveParams, wavCLVCurve, wavdenoise, wavdenoiseh, wavblcurve, waOpacityCurveRG, waOpacityCurveSH, waOpacityCurveBY, waOpacityCurveW, waOpacityCurveWL, wavclCurve, scale);


                if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari") && WaveParams.expclari && WaveParams.CLmethod != "all") {
                    WaveParams.expcontrast = procont;
                    WaveParams.expchroma = prochro;
                    WaveParams.expedge = proedge;
                    WaveParams.expfinal = profin;
                    WaveParams.exptoning = proton;
                    WaveParams.expnoise = pronois;

                    if (WaveParams.softrad > 0.f) {

                        array2D<float> ble(pW, pH);
                        array2D<float> guid(pW, pH);
                        Imagefloat *tmpImage = nullptr;
                        tmpImage = new Imagefloat(pW, pH);

#ifdef _OPENMP
                        #pragma omp parallel for
#endif

                        for (int ir = 0; ir < pH; ir++)
                            for (int jr = 0; jr < pW; jr++) {
                                float X, Y, Z;
                                float L = provradius->L[ir][jr];
                                float a = provradius->a[ir][jr];
                                float b = provradius->b[ir][jr];
                                Color::Lab2XYZ(L, a, b, X, Y, Z);

                                guid[ir][jr] = Y / 32768.f;
                                float La = nprevl->L[ir][jr];
                                float aa = nprevl->a[ir][jr];
                                float ba = nprevl->b[ir][jr];
                                Color::Lab2XYZ(La, aa, ba, X, Y, Z);
                                tmpImage->r(ir, jr) = X;
                                tmpImage->g(ir, jr) = Y;
                                tmpImage->b(ir, jr) = Z;
                                ble[ir][jr] = Y / 32768.f;
                            }

                        double epsilmax = 0.0001;
                        double epsilmin = 0.00001;
                        double aepsil = (epsilmax - epsilmin) / 100.f;
                        double bepsil = epsilmin; //epsilmax - 100.f * aepsil;
                        double epsil = aepsil * WaveParams.softrad + bepsil;

                        float blur = 10.f / scale * (0.5f + 0.8f * WaveParams.softrad);
                        // rtengine::guidedFilter(guid, ble, ble, blur, 0.001, multiTh);
                        rtengine::guidedFilter(guid, ble, ble, blur, epsil, false);



#ifdef _OPENMP
                        #pragma omp parallel for
#endif

                        for (int ir = 0; ir < pH; ir++)
                            for (int jr = 0; jr < pW; jr++) {
                                float X = tmpImage->r(ir, jr);
                                float Y = 32768.f * ble[ir][jr];
                                float Z = tmpImage->b(ir, jr);
                                float L, a, b;
                                Color::XYZ2Lab(X, Y, Z, L, a, b);
                                nprevl->L[ir][jr] =  L;
                            }

                        delete tmpImage;

                    }

                }

                if ((WaveParams.ushamethod == "sharp" || WaveParams.ushamethod == "clari")  && WaveParams.expclari && WaveParams.CLmethod != "all") {
                    float mL = (float)(WaveParams.mergeL / 100.f);
                    float mC = (float)(WaveParams.mergeC / 100.f);
                    float mL0;
                    float mC0;
                    float background = 0.f;
                    int show = 0;



                    if ((WaveParams.CLmethod == "one" || WaveParams.CLmethod == "inf")  && WaveParams.Backmethod == "black") {
                        mL0 = mC0 = 0.f;
                        mL = - 1.5f * mL;
                        mC = -mC;
                        background = 12000.f;
                        show = 0;
                    } else if (WaveParams.CLmethod == "sup" && WaveParams.Backmethod == "resid") {
                        mL0 = mL;
                        mC0 = mC;
                        background = 0.f;
                        show = 0;
                    } else {
                        mL0 = mL = mC0 = mC = 0.f;
                        background = 0.f;
                        show = 0;
                    }

                    float indic = 1.f;

                    if (WaveParams.showmask) {
                        mL0 = mC0 = -1.f;
                        indic = -1.f;
                        mL = fabs(mL);
                        mC = fabs(mC);
                        show = 1;
                    }

#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for (int x = 0; x < pH; x++)
                        for (int y = 0; y < pW; y++) {
                            nprevl->L[x][y] = LIM((1.f + mL0) * (unshar->L[x][y]) + show * background - mL * indic * nprevl->L[x][y], 0.f, 32768.f);
                            nprevl->a[x][y] = (1.f + mC0) * (unshar->a[x][y]) - mC * indic * nprevl->a[x][y];
                            nprevl->b[x][y] = (1.f + mC0) * (unshar->b[x][y]) - mC * indic * nprevl->b[x][y];
                        }

                    delete unshar;
                    unshar    = NULL;


                    /*
                                        if (WaveParams.softrad > 0.f) {
                                            array2D<float> ble(pW, pH);
                                            array2D<float> guid(pW, pH);
                    #ifdef _OPENMP
                                            #pragma omp parallel for
                    #endif

                                            for (int ir = 0; ir < pH; ir++)
                                                for (int jr = 0; jr < pW; jr++) {
                                                    ble[ir][jr] = (nprevl->L[ir][jr]  - provradius->L[ir][jr]) / 32768.f;
                                                    guid[ir][jr] = provradius->L[ir][jr] / 32768.f;
                                                }
                                            double epsilmax = 0.001;
                                            double epsilmin = 0.0001;
                                            double aepsil = (epsilmax - epsilmin) / 90.f;
                                            double bepsil = epsilmax - 100.f * aepsil;
                                            double epsil = aepsil * WaveParams.softrad + bepsil;

                                            float blur = 10.f / scale * (0.001f + 0.8f * WaveParams.softrad);
                                            // rtengine::guidedFilter(guid, ble, ble, blur, 0.001, multiTh);
                                            rtengine::guidedFilter(guid, ble, ble, blur, epsil, false);



                    #ifdef _OPENMP
                                            #pragma omp parallel for
                    #endif

                                            for (int ir = 0; ir < pH; ir++)
                                                for (int jr = 0; jr < pW; jr++) {
                                                    nprevl->L[ir][jr] =  provradius->L[ir][jr] + 32768.f * ble[ir][jr];
                                                }
                                        }
                    */
                    if (WaveParams.softrad > 0.f) {

                        delete provradius;
                        provradius    = NULL;

                    }


                }

            }

            ipf.softLight(nprevl, params->softlight);


            if (params->icm.workingTRC != ColorManagementParams::WorkingTrc::NONE && params->icm.trcExp) {
                const int GW = nprevl->W;
                const int GH = nprevl->H;
                std::unique_ptr<LabImage> provis;
                const float pres = 0.01f * params->icm.preser;
                if(params->icm.trcExp) {//local contrast
                    int level_hr = 7;
                    int maxlevpo = 9;
                    bool wavcurvecont = false;
                    WaveletParams WaveParams = params->wavelet;
                    ColorManagementParams Colparams = params->icm;
                    Colparams.getCurves(icmOpacityCurveWL);
                    ipf.complete_local_contrast(nprevl, nprevl, WaveParams,Colparams, icmOpacityCurveWL, scale, level_hr, maxlevpo, wavcurvecont);
                }

                if (pres > 0.f && params->icm.wprim != ColorManagementParams::Primaries::DEFAULT) {
                    provis.reset(new LabImage(GW, GH));
                    provis->CopyFrom(nprevl);
                }

                std::unique_ptr<Imagefloat> tmpImage1(new Imagefloat(GW, GH));
                std::unique_ptr<Imagefloat> tmpImage2(new Imagefloat(GW, GH));



                ipf.lab2rgb(*nprevl, *tmpImage1, params->icm.workingProfile);
                tmpImage1.get()->copyData(tmpImage2.get());

                const float gamtone = params->icm.wGamma;
                const float slotone = params->icm.wSlope;
                int illum = toUnderlying(params->icm.will);
                const int prim = toUnderlying(params->icm.wprim);

                Glib::ustring prof = params->icm.workingProfile;
                
                cmsHTRANSFORM dummy = nullptr;
                int ill = 0;
                bool gamutcontrol = params->icm.gamut;
                int catc = toUnderlying(params->icm.wcat);
                int locprim = 0;
                float rdx, rdy, grx, gry, blx, bly = 0.f;
                float meanx, meany, meanxe, meanye, maxdat = 0.f;
                const int midton = params->icm.wmidtcie;
                if(midton != 0) {
                    ToneEqualizerParams params;
                    params.enabled = true;
                    params.regularization = 0.f;
                    params.pivot = 0.f;
                    params.bands[0] = 0;
                    params.bands[2] = midton;
                    params.bands[4] = 0;
                    params.bands[5] = 0;
                    int mid = abs(midton);
                    int threshmid = 50;
                    if(mid > threshmid) {
                        params.bands[1] = sign(midton) * (mid - threshmid);
                        params.bands[3] = sign(midton) * (mid - threshmid);     
                    }
                    ipf.toneEqualizer(tmpImage1.get(), params, prof, scale, false);
                }
                double p[6] = {0., 0., 0., 0., 0., 0.};

                ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, -5, prof, 2.4, 12.92310, 0, ill, 0, 0,  rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat,  p, dummy, true, false, false, false);
                ipf.workingtrc(0, tmpImage1.get(), tmpImage1.get(), GW, GH, 5, prof, gamtone, slotone, catc, illum, prim, locprim,  rdx, rdy, grx, gry, blx, bly, meanx, meany, meanxe, meanye, maxdat, p, dummy, false, true, true, gamutcontrol);
                float satu = params->icm.wapsat;
                if(satu > 0.f) {
                    ipf.apsatur(0, tmpImage1.get(), tmpImage2.get(), GW, GH, satu) ;      
                }
 
                const float smoothisli = params->icm.wsmoothciesli;
                if(smoothisli > 0.f) {
                    ToneEqualizerParams params;
                    params.enabled = true;
                    params.regularization = 0.f;
                    params.pivot = 0.f;
                    params.bands[0] = 0;
                    params.bands[1] = 0;
                    params.bands[2] = 0;
                    params.bands[3] = 0;
                    params.bands[4] = -40;//arbitrary value to adapt with WhiteEvjz - here White Ev # 10
                    params.bands[5] = -80;//8 Ev and above
                    bool Evsix = true;
                    if(Evsix) {//EV = 6 majority of images
                        params.bands[4] = -30 * smoothisli;
                        float smmothsli5 = std::min(smoothisli, 1.f);
                        params.bands[5] = -80 * smmothsli5;
                    }               
                    ipf.toneEqualizer(tmpImage1.get(), params, prof, scale, false);
                }

                float maxdata = 0.f;
#ifdef _OPENMP
        #   pragma omp parallel for reduction(max:maxdata)
#endif
                for (int i = 0; i < GH; ++i){
                    for (int j = 0; j < GW; ++j) {
                        const float r = tmpImage1->r(i, j);
                        const float g = tmpImage1->g(i, j);
                        const float b = tmpImage1->b(i, j);
                        float maxrgb2 = rtengine::max(r, g, b);
                        if(maxrgb2 > maxdata){
                            maxdata = maxrgb2;
                        }
                    }
                }
                maxdat = maxdata / 65535.f;

                if (primListener) {
                    primListener->maxdatawtrc(maxdat);
                }

                ipf.rgb2lab(*tmpImage1, *nprevl, params->icm.workingProfile);

                //nprevl and provis
                if (provis) {
                    ipf.preserv(nprevl, provis.get(), GW, GH);
                }

                if (params->icm.fbw) {
#ifdef _OPENMP
                    #pragma omp parallel for
#endif

                    for (int x = 0; x < GH; x++)
                        for (int y = 0; y < GW; y++) {
                            nprevl->a[x][y] = 0.f;
                            nprevl->b[x][y] = 0.f;
                        }
                }

                tmpImage1.reset();

                if (prim == 14) {//pass red gre blue xy in function of area data Ciexy  - prim = 14 correspond to ColorManagementParams::Primaries::CUSTOM_GRID
                    float redgraphx =  params->icm.labgridcieALow;
                    float redgraphy =  params->icm.labgridcieBLow;
                    float blugraphx =  params->icm.labgridcieAHigh;
                    float blugraphy =  params->icm.labgridcieBHigh;
                    float gregraphx =  params->icm.labgridcieGx;
                    float gregraphy =  params->icm.labgridcieGy;
                    constexpr float INV_OFFSET_MODIFIER = 1.f / OFFSET_MODIFIER;
                    float redxx = INV_OFFSET_MODIFIER * (redgraphx + 1.f) - CIExy_MARGIN;
                    redxx = rtengine::LIM(redxx, 0.41f, 1.f);
                    float redyy = INV_OFFSET_MODIFIER * (redgraphy + 1.f) - CIExy_MARGIN;
                    redyy = rtengine::LIM(redyy, 0.f, 0.7f);
                    float bluxx = INV_OFFSET_MODIFIER * (blugraphx + 1.f) - CIExy_MARGIN;
                    bluxx = rtengine::LIM(bluxx, -0.1f, 0.5f);
                    float bluyy = INV_OFFSET_MODIFIER * (blugraphy + 1.f) - CIExy_MARGIN;
                    bluyy = rtengine::LIM(bluyy, -0.1f, 0.5f);
                    float grexx = INV_OFFSET_MODIFIER * (gregraphx + 1.f) - CIExy_MARGIN;
                    grexx = rtengine::LIM(grexx, -0.1f, 0.4f);
                    float greyy = INV_OFFSET_MODIFIER * (gregraphy + 1.f) - CIExy_MARGIN;
                    greyy = rtengine::LIM(greyy, 0.5f, 1.f);
                    if (primListener) {
                        primListener->primChanged(redxx, redyy, bluxx, bluyy, grexx, greyy);
                    }
                } else {//all other cases - pass Cie xy to update graph Ciexy
                    float r_x =  params->icm.redx;
                    float r_y =  params->icm.redy;
                    float b_x =  params->icm.blux;
                    float b_y =  params->icm.bluy;
                    float g_x =  params->icm.grex;
                    float g_y =  params->icm.grey;
                    //printf("rx=%f ry=%f \n", (double) r_x, (double) r_y);
                    float wx = 0.33f;
                    float wy = 0.33f;

                    switch (illum) {
                        case 1://D41
                            wx = 0.37798f;
                            wy = 0.38123f;
                            break;

                        case 2://D50
                            wx = 0.3457f;
                            wy = 0.3585f;
                            break;

                        case 3://D55
                            wx = 0.3324f;
                            wy = 0.3474f;
                            break;

                        case 4://D60
                            wx = 0.3217f;
                            wy = 0.3377f;
                            break;

                        case 5://D65
                            wx = 0.3127f;
                            wy = 0.3290f;
                            break;

                        case 6://D80
                            wx = 0.2937f;
                            wy = 0.3092f;
                            break;

                        case 7://D120
                            wx = 0.2697f;
                            wy = 0.2808f;
                            break;

                        case 8://stdA
                            wx = 0.4476f;
                            wy = 0.4074f;
                            break;

                        case 9://2000K
                            wx = 0.5266f;
                            wy = 0.4133f;
                            break;

                        case 10://1500K
                            wx = 0.5857f;
                            wy = 0.3932f;
                            break;
                    }
                    
                   
                    //move white point in GUI
                    double refin = params->icm.refi;
                    double arefi = (wy - meany) / (wx - meanx);
                    double brefi = wy - arefi * wx;
                    double scalrefi = meanx - wx;
                    wx = wx + scalrefi * refin;
                    wy = wx * arefi + brefi;

                    if (primListener) {
                        if( params->icm.wprim != ColorManagementParams::Primaries::CUSTOM_POL) {
                            primListener->iprimChanged(r_x, r_y, b_x, b_y, g_x, g_y, wx, wy, meanx, meany);
                        } else {
                            primListener->iprimChanged(p[0], p[1], p[4] ,p[5], p[2], p[3], wx, wy, meanx, meany);
                        }
                    }
                }
            }

            if (params->colorappearance.enabled) {
                // L histo  and Chroma histo for ciecam
                // histogram will be for Lab (Lch) values, because very difficult to do with J,Q, M, s, C
                int x1, y1, x2, y2;
                params->crop.mapToResized(pW, pH, scale, x1, x2,  y1, y2);
                lhist16CAM.clear();
                lhist16CCAM.clear();

                if (!params->colorappearance.datacie) {
                    for (int x = 0; x < pH; x++)
                        for (int y = 0; y < pW; y++) {
                            int pos = CLIP((int)(nprevl->L[x][y]));
                            int posc = CLIP((int)sqrt(nprevl->a[x][y] * nprevl->a[x][y] + nprevl->b[x][y] * nprevl->b[x][y]));
                            lhist16CAM[pos]++;
                            lhist16CCAM[posc]++;
                        }
                }

                CurveFactory::curveLightBrightColor(params->colorappearance.curve, params->colorappearance.curvered, params->colorappearance.curvegreen, params->colorappearance.curveblue, params->colorappearance.curve2, params->colorappearance.curve3,
                                                    lhist16CAM, histLCAM, lhist16CCAM, histCCAM,
                                                    customColCurve1, customColCurvered, customColCurvegreen, customColCurveblue, customColCurve2, customColCurve3, 1);

                const FramesMetaData* metaData = imgsrc->getMetaData();
                float fnum = metaData->getFNumber();          // F number
                float fiso = metaData->getISOSpeed() ;        // ISO
                float fspeed = metaData->getShutterSpeed() ;  // Speed
                double fcomp = metaData->getExpComp();        // Compensation +/-
                double adap;

                if (fnum < 0.3f || fiso < 5.f || fspeed < 0.00001f) { //if no exif data or wrong
                    adap = 2000.;
                } else {
                    double E_V = fcomp + log2(double ((fnum * fnum) / fspeed / (fiso / 100.f)));
                    double kexp = 0.;
                    E_V += kexp * params->toneCurve.expcomp;// exposure compensation in tonecurve ==> direct EV
                    E_V += 0.5 * log2(params->raw.expos);  // exposure raw white point ; log2 ==> linear to EV
                    adap = pow(2.0, E_V - 3.0);  // cd / m2
                    // end calculation adaptation scene luminosity
                }

                if (params->colorappearance.catmethod == "symg") { //force abolute luminance scenescene to 400 in symmetric
                    adap = 400.;
                }

                float d, dj, yb;
                bool execsharp = false;

                if (!ncie) {
                    ncie = new CieImage(pW, pH);
                }

                if (!CAMBrightCurveJ && (params->colorappearance.algo == "JC" || params->colorappearance.algo == "JS" || params->colorappearance.algo == "ALL")) {
                    CAMBrightCurveJ(32768, 0);
                }

                if (!CAMBrightCurveQ && (params->colorappearance.algo == "QM" || params->colorappearance.algo == "ALL")) {
                    CAMBrightCurveQ(32768, 0);
                }

                // Issue 2785, only float version of ciecam02 for navigator and pan background
                CAMMean = NAN;
                CAMBrightCurveJ.dirty = true;
                CAMBrightCurveQ.dirty = true;

                ipf.ciecam_02float(ncie, float (adap), pW, 2, nprevl, params.get(), customColCurve1, customColCurvered, customColCurvegreen, customColCurveblue, customColCurve2, customColCurve3, histLCAM, histCCAM, CAMBrightCurveJ, CAMBrightCurveQ, CAMMean, 0, scale, execsharp, d, dj, yb, 1);

                //call listener
                if ((params->colorappearance.autodegree || params->colorappearance.autodegreeout) && acListener && params->colorappearance.enabled) {
                    if (params->colorappearance.catmethod == "symg") { //force chromatic adaptation to 90 in symmetric
                        d = 0.9;
                        dj = 0.9;
                    }

                    acListener->autoCamChanged(100.* (double)d, 100.* (double)dj);
                }

                if (params->colorappearance.autoadapscen && acListener && params->colorappearance.enabled) {
                    acListener->adapCamChanged(adap);    //real value of adapt scene, force to 400 in symmetric
                }

                if (params->colorappearance.autoybscen && acListener && params->colorappearance.enabled) {
                    if (params->colorappearance.catmethod == "symg") { //force yb scene to 18 in symmetric
                        yb = 18;
                    }

                    acListener->ybCamChanged((int) yb);    //real value Yb scene
                }

                double tempsym = 5003.;
                double greensym = 1.;
                int wmodel = 0;//wmodel allows - arbitrary - choice of illuminant and temp with choice

                if (params->colorappearance.wbmodel == "RawT") {
                    wmodel = 0;
                } else if (params->colorappearance.wbmodel == "RawTCAT02") {
                    wmodel = 1;
                } else if (params->colorappearance.wbmodel == "free") {
                    wmodel = 2;//force white balance in symmetric
                }

                if (params->colorappearance.catmethod == "symg" && wmodel == 2) {
                    tempsym = params->wb.temperature;//force white balance in symmetric
                } else if(params->colorappearance.autotempout) {
                    if (params->colorappearance.illum == "iA") {//otherwise force illuminant source
                        tempsym = 2856.;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "i41") {
                        tempsym = 4100.;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "i50") {
                        tempsym = 5003.;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "i55") {
                        tempsym = 5503.;
                    } else if (params->colorappearance.illum == "i60") {
                        tempsym = 6000. ;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "i65") {
                        tempsym = 6504.;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "i75") {
                        tempsym = 7504.;
                        greensym = 1.;
                    } else if (params->colorappearance.illum == "ifree") {
                        tempsym = params->wb.temperature;//force white balance in symmetric
                        greensym = 1.;
                    }
                } else {
                    tempsym = params->colorappearance.tempout;
                    greensym = params->colorappearance.greenout;
                }
                if (params->colorappearance.enabled  && acListener) {
                    acListener->wbCamChanged(tempsym, greensym, params->colorappearance.autotempout);    //real temp and tint.
                }

            } else {
                // CIECAM is disabled, we free up its image buffer to save some space
                if (ncie) {
                    delete ncie;
                }

                ncie = nullptr;

                if (CAMBrightCurveJ) {
                    CAMBrightCurveJ.reset();
                }

                if (CAMBrightCurveQ) {
                    CAMBrightCurveQ.reset();
                }
            }
        
            bool exec = params->icm.wgamut != ColorManagementParams::Wwgamut::NONE  || params->icm.wgamgain != 0.f;
            bool gamgain = false;
            float maxdatend = 0.f;
            float satdatend = 0.f;
           
            if (params->icm.workingTRC != ColorManagementParams::WorkingTrc::NONE && params->icm.trcExp  && exec) {
                //compression gamut and gain at the end of process
                gamgain = true;

                const int GW = nprevl->W;
                const int GH = nprevl->H;
                TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params->icm.workingProfile);
                    const double wp[3][3] = {
                        {wprof[0][0], wprof[0][1], wprof[0][2]},
                        {wprof[1][0], wprof[1][1], wprof[1][2]},
                        {wprof[2][0], wprof[2][1], wprof[2][2]}
                    };
                TMatrix wiprof = ICCStore::getInstance()->workingSpaceInverseMatrix(params->icm.workingProfile);
                    const double wip[3][3] = {//improve precision with double
                        {wiprof[0][0], wiprof[0][1], wiprof[0][2]},
                        {wiprof[1][0], wiprof[1][1], wiprof[1][2]},
                        {wiprof[2][0], wiprof[2][1], wiprof[2][2]}
                    };

                std::unique_ptr<Imagefloat> provcomp(new Imagefloat(GW, GH));

#ifdef _OPENMP
        #   pragma omp parallel for
#endif
                for (int i = 0; i < GH; ++i){
                    for (int j = 0; j < GW; ++j) {
                        float X, Y, Z = 0.f;
                        Color::Lab2XYZ(nprevl->L[i][j], nprevl->a[i][j], nprevl->b[i][j] , X, Y, Z);
                        Color::xyz2rgb(X, Y, Z, provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), wip);
                    }
                }

                const float gainev = pow_F(2.f, (float) params->icm.wgamgain);
                if (params->icm.wgamgain != 0.f) {//Final gain in Ev
#ifdef _OPENMP
        #   pragma omp parallel for
#endif

                    for (int i = 0; i < GH; ++i){
                        for (int j = 0; j < GW; ++j) {
                            provcomp->r(i, j) *= gainev;
                            provcomp->g(i, j) *= gainev;
                            provcomp->b(i, j) *= gainev;
                        }
                    }
                }
                float mac = 0.f;
                float mac0 = 0.f;
                float mac1 = 0.f;
                float mac2 = 0.f;
                int beginend = 1;
                int nbsegam = 0;
                float powe = 1.f;
                if ( params->icm.wgamut != ColorManagementParams::Wwgamut::NONE) {
                    ipf.gamutcompr(provcomp.get(), provcomp.get(), beginend, powe, nbsegam, mac, mac0, mac1, mac2);
                }

                float rgbmax = 0.f;
                float satmax = 0.f;
#ifdef _OPENMP
        #   pragma omp parallel for reduction(max:rgbmax) reduction(max:satmax)
#endif
                for (int i = 0; i < GH; ++i){
                    for (int j = 0; j < GW; ++j) {
                        const float r = provcomp->r(i, j);
                        const float g = provcomp->g(i, j);
                        const float b = provcomp->b(i, j);
                        float maxrgbend = rtengine::max(r, g, b);
                        if(maxrgbend> rgbmax){//RGB Max
                            rgbmax = maxrgbend;
                        }
                        float h, s, l = 0.f;
                        Color::rgb2hsl(r, g, b, h, s, l);
                        float maxsatend = s;
                        if(maxsatend> satmax){//Saturation max
                            satmax = maxsatend;
                        }
                    }
                }
                maxdatend = rgbmax / 65535.f;
                satdatend = satmax;

#ifdef _OPENMP
        #   pragma omp parallel for
#endif
                for (int i = 0; i < GH; ++i){
                    for (int j = 0; j < GW; ++j) {
                        float x, y, z = 0.f;
                        Color::rgbxyz (provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), x, y, z, wp);
                        Color::XYZ2Lab(x, y, z, nprevl->L[i][j], nprevl->a[i][j], nprevl->b[i][j]);
                    }
                }
            }
            if (primListener) {
                primListener->maxdataend(maxdatend, satdatend, gamgain);
            }

        }
        //  if (todo & (M_AUTOEXP | M_RGBCURVE)) {

        // Update the monitor color transform if necessary
        if ((todo & M_MONITOR) || (lastOutputProfile != params->icm.outputProfile) || lastOutputIntent != params->icm.outputIntent || lastOutputBPC != params->icm.outputBPC) {
            lastOutputProfile = params->icm.outputProfile;
            lastOutputIntent = params->icm.outputIntent;
            lastOutputBPC = params->icm.outputBPC;
            ipf.updateColorProfiles(monitorProfile, monitorIntent, softProof, gamutCheck);
        }
    }

// process crop, if needed
    if (!suppressDeferredCropRender) {
        renderCrops(todo, panningRelatedChange
                        || (highDetailNeeded && prevdemo != PD_Sidecar));
    }

    if (panningRelatedChange || (todo & M_MONITOR)) {
        if ((todo != CROP && todo != MINUPDATE) || (todo & M_MONITOR)) {
            MyMutex::MyLock prevImgLock(previmg->getMutex());

            try {
                // Computing the preview image, i.e. converting from WCS->Monitor color space (soft-proofing disabled) or WCS->Printer profile->Monitor color space (soft-proofing enabled)
                ipf.lab2monitorRgb(nprevl, previmg);

                // Computing the internal image for analysis, i.e. conversion from WCS->Output profile
                delete workimg;
                workimg = nullptr;

                workimg = ipf.lab2rgb(nprevl, 0, 0, pW, pH, params->icm);
            } catch (std::exception&) {
                return;
            }

            if (preview_dump::enabled()) {
                preview_dump::dump_full_frame(previmg, workimg, scale, todo);
            }
        }

        if (!resultValid) {
            resultValid = true;

            if (imageListener) {
                imageListener->setImage(previmg, scale, params->crop, params->cropGuide);
                LoadProfile::markFirst(
                    "preview setImage",
                    LoadProfile::state().setImageSeen);
            }
        }

        if (imageListener)
            // TODO: The WB tool should be advertised too in order to get the AutoWB's temp and green values
        {
            imageListener->imageReady(params->crop, params->cropGuide);
            LoadProfile::markFirst(
                "preview imageReady",
                LoadProfile::state().imageReadySeen);
        }

        hist_lrgb_dirty = vectorscope_hc_dirty = vectorscope_hs_dirty = waveform_dirty = true;

        if (hListener) {
            if (hListener->updateHistogram()) {
                updateLRGBHistograms();
            }

            if (hListener->updateVectorscopeHC()) {
                updateVectorscopeHC();
            }

            if (hListener->updateVectorscopeHS()) {
                updateVectorscopeHS();
            }

            if (hListener->updateWaveform()) {
                updateWaveforms();
            }

            notifyHistogramChanged();
        }
    }

    if (orig_prev != oprevi) {
        delete oprevi;
        oprevi = nullptr;
    }

    if (fullPipelineRan) {
        markViewportStateValid(highDetailNeeded);
    } else {
        // This request did not rebuild the base/analysis state. Never publish
        // it as a valid snapshot, and do not leave a stale deferred request
        // blocking the next fast path forever.
        forceFullPreview.store(false, std::memory_order_relaxed);
        viewportStateValid.store(false, std::memory_order_relaxed);
        deferredCropRendered.store(false, std::memory_order_relaxed);
    }
}

void ImProcCoordinator::setTweakOperator(TweakOperator *tOperator)
{
    if (tOperator) {
        tweakOperator = tOperator;
        // The tweak changes how the next pass renders the parameters, so an
        // analysis accumulated for the untweaked inputs must not be flushed
        // with the tweak applied. The tool's refresh request rebuilds it.
        dropPendingDeferred();
    }
}

void ImProcCoordinator::unsetTweakOperator(TweakOperator *tOperator)
{
    if (tOperator && tOperator == tweakOperator) {
        tweakOperator = nullptr;
        dropPendingDeferred();
    }
}

void ImProcCoordinator::freeAll()
{
    previewBaseCache->clear("free-all");
    invalidateViewportState();

    if (allocated) {
        if (spotprev && spotprev != oprevi) {
            delete spotprev;
        }

        spotprev = nullptr;

        if (orig_prev != oprevi) {
            delete oprevi;
        }

        oprevi    = nullptr;
        delete orig_prev;
        orig_prev = nullptr;
        delete oprevl;
        oprevl    = nullptr;
        delete nprevl;
        nprevl    = nullptr;

        if (ncie) {
            delete ncie;
        }

        ncie      = nullptr;

        if (imageListener) {
            imageListener->delImage(previmg);
        } else {
            delete previmg;
        }

        delete workimg;
        workimg = nullptr;

    }

    allocated = false;
}

void ImProcCoordinator::allocCache(Imagefloat* &imgfloat)
{
    if (imgfloat == nullptr) {
        imgfloat = new Imagefloat(pW, pH);
    } else {
        imgfloat->allocate(pW, pH);
    }
}

/** @brief Handles image buffer (re)allocation and trigger sizeChanged of SizeListener[s]
 * If the scale change, this method will free all buffers and reallocate ones of the new size.
 * It will then tell to the SizeListener that size has changed (sizeChanged)
 *
 * @param prevscale New Preview's scale.
 */
void ImProcCoordinator::setScale(int prevscale, bool allowSmall)
{
    const int oldScale = scale;

    tr = getCoarseBitMask(params->coarse);

    int nW, nH;
    imgsrc->getFullSize(fw, fh, tr);

    prevscale = std::max(2, prevscale + 1);

    do {
        prevscale--;
        PreviewProps pp(0, 0, fw, fh, prevscale);
        imgsrc->getSize(pp, nW, nH);
    } while (!allowSmall && nH < 400 && prevscale > 1
             && (nW * nH < 1000000));  // actually hardcoded values, perhaps a better choice is possible

    if (nW != pW || nH != pH) {

        freeAll();

        pW = nW;
        pH = nH;

        orig_prev = new Imagefloat(pW, pH);
        oprevi = orig_prev;
        oprevl = new LabImage(pW, pH);
        nprevl = new LabImage(pW, pH);

        //ncie is only used in ImProcCoordinator::updatePreviewImage, it will be allocated on first use and deleted if not used anymore
        previmg = new Image8(pW, pH);
        workimg = new Image8(pW, pH);

        allocated = true;
    } else if (prevscale != oldScale) {
        previewBaseCache->clear("scale-change");
        invalidateViewportState();
    }

    scale = prevscale;
    resultValid = false;
    fullw = fw;
    fullh = fh;

    if (!sizeListeners.empty())
        for (size_t i = 0; i < sizeListeners.size(); i++) {
            sizeListeners[i]->sizeChanged(fullw, fullh, fw, fh);
        }
}


void ImProcCoordinator::notifyHistogramChanged()
{
    if (hListener) {
        hListener->histogramChanged(
            histRed,
            histGreen,
            histBlue,
            histLuma,
            histToneCurve,
            histLCurve,
            histCCurve,
            histLCAM,
            histCCAM,
            histRedRaw,
            histGreenRaw,
            histBlueRaw,
            histChroma,
            histLRETI,
            vectorscopeScale,
            vectorscope_hc,
            vectorscope_hs,
            waveformScale,
            waveformRed,
            waveformGreen,
            waveformBlue,
            waveformLuma
        );
    }
}

bool ImProcCoordinator::updateLRGBHistograms()
{

    if (!hist_lrgb_dirty) {
        return false;
    }

    int x1, y1, x2, y2;
    params->crop.mapToResized(pW, pH, scale, x1, x2, y1, y2);

#ifdef _OPENMP
    #pragma omp parallel sections
#endif
    {
#ifdef _OPENMP
        #pragma omp section
#endif
        {
            histChroma.clear();

            for (int i = y1; i < y2; i++)
                for (int j = x1; j < x2; j++)
                {
                    histChroma[(int)(sqrtf(SQR(nprevl->a[i][j]) + SQR(nprevl->b[i][j])) / 188.f)]++;      //188 = 48000/256
                }
        }
#ifdef _OPENMP
        #pragma omp section
#endif
        {
            histLuma.clear();

            for (int i = y1; i < y2; i++)
                for (int j = x1; j < x2; j++)
                {
                    histLuma[(int)(nprevl->L[i][j] / 128.f)]++;
                }
        }
#ifdef _OPENMP
        #pragma omp section
#endif
        {
            histRed.clear();
            histGreen.clear();
            histBlue.clear();

            for (int i = y1; i < y2; i++)
            {
                int ofs = (i * pW + x1) * 3;

                for (int j = x1; j < x2; j++) {
                    int r = workimg->data[ofs++];
                    int g = workimg->data[ofs++];
                    int b = workimg->data[ofs++];

                    histRed[r]++;
                    histGreen[g]++;
                    histBlue[b]++;
                }
            }
        }
    }

    hist_lrgb_dirty = false;
    return true;

}

bool ImProcCoordinator::updateVectorscopeHC()
{
    if (!workimg || !vectorscope_hc_dirty) {
        return false;
    }

    int x1, y1, x2, y2;
    params->crop.mapToResized(pW, pH, scale, x1, x2, y1, y2);

    constexpr int size = VECTORSCOPE_SIZE;
    constexpr float norm_factor = size / (128.f * 655.36f);
    vectorscope_hc.fill(0);

    vectorscopeScale = (x2 - x1) * (y2 - y1);

    const std::unique_ptr<float[]> a(new float[vectorscopeScale]);
    const std::unique_ptr<float[]> b(new float[vectorscopeScale]);
    const std::unique_ptr<float[]> L(new float[vectorscopeScale]);
    ipf.rgb2lab(*workimg, x1, y1, x2 - x1, y2 - y1, L.get(), a.get(), b.get(), params->icm);
#ifdef _OPENMP
    #pragma omp parallel
#endif
    {
        array2D<int> vectorscopeThr(size, size, ARRAY2D_CLEAR_DATA);
#ifdef _OPENMP
        #pragma omp for nowait
#endif

        for (int i = y1; i < y2; ++i) {
            for (int j = x1, ofs_lab = (i - y1) * (x2 - x1); j < x2; ++j, ++ofs_lab) {
                const int col = norm_factor * a[ofs_lab] + size / 2 + 0.5f;
                const int row = norm_factor * b[ofs_lab] + size / 2 + 0.5f;

                if (col >= 0 && col < size && row >= 0 && row < size) {
                    vectorscopeThr[row][col]++;
                }
            }
        }

#ifdef _OPENMP
        #pragma omp critical
#endif
        {
            vectorscope_hc += vectorscopeThr;
        }
    }

    vectorscope_hc_dirty = false;
    return true;
}

bool ImProcCoordinator::updateVectorscopeHS()
{
    if (!workimg || !vectorscope_hs_dirty) {
        return false;
    }

    int x1, y1, x2, y2;
    params->crop.mapToResized(pW, pH, scale, x1, x2, y1, y2);

    constexpr int size = VECTORSCOPE_SIZE;
    vectorscope_hs.fill(0);

    vectorscopeScale = (x2 - x1) * (y2 - y1);

#ifdef _OPENMP
    #pragma omp parallel
#endif
    {
        array2D<int> vectorscopeThr(size, size, ARRAY2D_CLEAR_DATA);
#ifdef _OPENMP
        #pragma omp for nowait
#endif

        for (int i = y1; i < y2; ++i) {
            int ofs = (i * pW + x1) * 3;

            for (int j = x1; j < x2; ++j) {
                const float red = 257.f * workimg->data[ofs++];
                const float green = 257.f * workimg->data[ofs++];
                const float blue = 257.f * workimg->data[ofs++];
                float h, s, l;
                Color::rgb2hslfloat(red, green, blue, h, s, l);
                const auto sincosval = xsincosf(2.f * RT_PI_F * h);
                const int col = s * sincosval.y * (size / 2) + size / 2;
                const int row = s * sincosval.x * (size / 2) + size / 2;

                if (col >= 0 && col < size && row >= 0 && row < size) {
                    vectorscopeThr[row][col]++;
                }
            }
        }

#ifdef _OPENMP
        #pragma omp critical
#endif
        {
            vectorscope_hs += vectorscopeThr;
        }
    }

    vectorscope_hs_dirty = false;
    return true;
}

bool ImProcCoordinator::updateWaveforms()
{
    if (!workimg) {
        // free memory
        waveformRed.free();
        waveformGreen.free();
        waveformBlue.free();
        waveformLuma.free();
        return true;
    }

    if (!waveform_dirty) {
        return false;
    }

    int x1, y1, x2, y2;
    params->crop.mapToResized(pW, pH, scale, x1, x2, y1, y2);
    int waveform_width = waveformRed.getWidth();

    if (waveform_width != x2 - x1) {
        // Resize waveform arrays.
        waveform_width = x2 - x1;
        waveformRed(waveform_width, 256);
        waveformGreen(waveform_width, 256);
        waveformBlue(waveform_width, 256);
        waveformLuma(waveform_width, 256);
    }

    // Start with zero.
    waveformRed.fill(0);
    waveformGreen.fill(0);
    waveformBlue.fill(0);
    waveformLuma.fill(0);

    constexpr float luma_factor = 255.f / 32768.f;

    for (int i = y1; i < y2; i++) {
        int ofs = (i * pW + x1) * 3;
        float* L_row = nprevl->L[i] + x1;

        for (int j = 0; j < waveform_width; j++) {
            waveformRed[workimg->data[ofs++]][j]++;
            waveformGreen[workimg->data[ofs++]][j]++;
            waveformBlue[workimg->data[ofs++]][j]++;
            waveformLuma[LIM<int>(L_row[j] * luma_factor, 0, 255)][j]++;
        }
    }

    waveformScale = y2 - y1;
    waveform_dirty = false;
    return true;
}

bool ImProcCoordinator::getAutoWB(double& temp, double& green, double equal, StandardObserver observer, double tempBias)
{

    if (imgsrc) {
        if (lastAwbEqual != equal || lastAwbObserver != observer || lastAwbTempBias != tempBias || lastAwbauto != params->wb.method) {
// Issue 2500            MyMutex::MyLock lock(minit);  // Also used in crop window
            double rm, gm, bm;
            params->wb.method = "autold";//same result as before multiple Auto WB

            // imgsrc->getAutoWBMultipliers(rm, gm, bm);
            double tempitc = 5000.;
            double greenitc = 1.;
            int dread = 0;
            int bia = 0;
            float temp0 = 5000.f;
            float studgood = 1000.f;
            int nocam = 0;
            int kcam = 0;
            float minchrom = 1000.f;
            float delta = 0.f;
            int kmin = 20;
            float minhist = 10000000.f;
            float maxhist = -1000.f;
            double tempref, greenref;
            bool extra = false;
            imgsrc->getAutoWBMultipliersitc(extra, tempref, greenref, tempitc, greenitc, temp0, delta, bia, dread, kcam, nocam, studgood, minchrom, kmin, minhist, maxhist, 0, 0, fh, fw, 0, 0, fh, fw, rm, gm, bm,  params->wb, params->icm, params->raw, params->toneCurve);

            if (rm != -1) {
                autoWB.update(rm, gm, bm, equal, observer, tempBias);
                lastAwbEqual = equal;
                lastAwbObserver = observer;
                lastAwbTempBias = tempBias;
                lastAwbauto = params->wb.method;
            } else {
                lastAwbEqual = -1.;
                lastAwbObserver = ColorTemp::DEFAULT_OBSERVER;
                autoWB.useDefaults(equal, observer);
                lastAwbauto = "";
                lastAwbTempBias = 0.0;
            }
        }

        temp = autoWB.getTemp();
        green = autoWB.getGreen();
        return true;
    } else {
        //temp = autoWB.getTemp();
        temp = -1.0;
        green = -1.0;
        return false;
    }
}

void ImProcCoordinator::getCamWB(double& temp, double& green, StandardObserver observer)
{

    if (imgsrc) {
        const ColorTemp color_temp = imgsrc->getWB().convertObserver(observer);
        temp = color_temp.getTemp();
        green = color_temp.getGreen();
    }
}

void ImProcCoordinator::getSpotWB(int x, int y, int rect, double& temp, double& tgreen)
{

    ColorTemp ret;

    {
        MyMutex::MyLock lock(mProcessing);
        std::vector<Coord2D> points, red, green, blue;

        for (int i = y - rect; i <= y + rect; i++)
            for (int j = x - rect; j <= x + rect; j++) {
                points.push_back(Coord2D(j, i));
            }

        ipf.transCoord(fw, fh, points, red, green, blue);

        int tr = getCoarseBitMask(params->coarse);

        ret = imgsrc->getSpotWB(red, green, blue, tr, params->wb.equal, params->wb.observer);
        currWB = ColorTemp(params->wb.temperature, params->wb.green, params->wb.equal, params->wb.method, params->wb.observer);
        //double rr,gg,bb;
        //currWB.getMultipliers(rr,gg,bb);

    } // end of mutex lockong

    if (ret.getTemp() > 0) {
        temp = ret.getTemp();
        tgreen = ret.getGreen();
    } else {
        temp = currWB.getTemp();
        tgreen = currWB.getGreen();
    }
}



void ImProcCoordinator::getAutoCrop(double ratio, int &x, int &y, int &w, int &h)
{

    MyMutex::MyLock lock(mProcessing);

    LensCorrection *pLCPMap = nullptr;

    if (params->lensProf.useLcp() && imgsrc->getMetaData()->getFocalLen() > 0) {
        const std::shared_ptr<LCPProfile> pLCPProf = LCPStore::getInstance()->getProfile(params->lensProf.lcpFile);

        if (pLCPProf) pLCPMap = new LCPMapper(pLCPProf, imgsrc->getMetaData()->getFocalLen(), imgsrc->getMetaData()->getFocalLen35mm(), imgsrc->getMetaData()->getFocusDist(),
                                                  0, false, params->lensProf.useDist, fullw, fullh, params->coarse, imgsrc->getRotateDegree());
    }

    double fillscale = ipf.getTransformAutoFill(fullw, fullh, pLCPMap);

    if (ratio > 0) {
        w = fullw * fillscale;
        h = w / ratio;

        if (h > fullh * fillscale) {
            h = fullh * fillscale;
            w = h * ratio;
        }
    } else {
        w = fullw * fillscale;
        h = fullh * fillscale;
    }

    x = (fullw - w) / 2;
    y = (fullh - h) / 2;
}

void ImProcCoordinator::setMonitorProfile(const Glib::ustring& profile, RenderingIntent intent)
{
    monitorProfile = profile;
    monitorIntent = intent;
}

void ImProcCoordinator::getMonitorProfile(Glib::ustring& profile, RenderingIntent& intent) const
{
    profile = monitorProfile;
    intent = monitorIntent;
}

void ImProcCoordinator::setSoftProofing(bool softProof, bool gamutCheck)
{
    this->softProof = softProof;
    this->gamutCheck = gamutCheck;
}

void ImProcCoordinator::getSoftProofing(bool &softProof, bool &gamutCheck)
{
    softProof = this->softProof;
    gamutCheck = this->gamutCheck;
}

ProcEvent ImProcCoordinator::setSharpMask(bool sharpMask)
{
    if (this->sharpMask != sharpMask) {
        sharpMaskChanged = true;
        this->sharpMask = sharpMask;
        return params->pdsharpening.enabled ? rtengine::EvPdShrMaskToggled : rtengine::EvShrEnabled;
    } else {
        sharpMaskChanged = false;
        return rtengine::EvShrEnabled;
    }
}

void ImProcCoordinator::saveInputICCReference(const Glib::ustring& fname, bool apply_wb)
{

    MyMutex::MyLock lock(mProcessing);

    // This path reprocesses and demosaics the shared ImageSource at full
    // resolution, so any preview base snapshot is no longer representative.
    previewBaseCache->markDirty("input-icc-reference");
    invalidateViewportState();
    highDetailPreprocessComputed = false;
    highDetailRawComputed = false;

    int fW, fH;
    std::unique_ptr<ProcParams> validParams(new ProcParams());
    getParams(validParams.get());

    int tr = getCoarseBitMask(validParams->coarse);

    imgsrc->getFullSize(fW, fH, tr);
    PreviewProps pp(0, 0, fW, fH, 1);
    ProcParams ppar = *validParams;
    ppar.toneCurve.hrenabled = false;
    ppar.icm.inputProfile = "(none)";
    Imagefloat* im = new Imagefloat(fW, fH);
    float reddeha = 0.f;
    float greendeha = 0.f;
    float bluedeha = 0.f;
    imgsrc->preprocess(ppar.raw, ppar.lensProf, ppar.coarse,reddeha, greendeha, bluedeha, true);
    double dummy = 0.0;
    imgsrc->demosaic(ppar.raw, false, dummy);
    ColorTemp currWB = ColorTemp(validParams->wb.temperature, validParams->wb.green, validParams->wb.equal, validParams->wb.method, validParams->wb.observer);

    if (validParams->wb.method == "Camera") {
        currWB = imgsrc->getWB();
    } else if (validParams->wb.method == "autold") {
        if (lastAwbEqual != validParams->wb.equal || lastAwbObserver != validParams->wb.observer || lastAwbTempBias != validParams->wb.tempBias) {
            double rm, gm, bm;
            imgsrc->getAutoWBMultipliers(rm, gm, bm);

            if (rm != -1.) {
                autoWB.update(rm, gm, bm, validParams->wb.equal, validParams->wb.observer, validParams->wb.tempBias);
                lastAwbEqual = validParams->wb.equal;
                lastAwbObserver = validParams->wb.observer;
                lastAwbTempBias = validParams->wb.tempBias;
            } else {
                lastAwbEqual = -1.;
                lastAwbObserver = ColorTemp::DEFAULT_OBSERVER;
                lastAwbTempBias = 0.0;
                autoWB.useDefaults(validParams->wb.equal, validParams->wb.observer);
            }
        }

        currWB = autoWB;
    }

    if (!apply_wb) {
        currWB = ColorTemp(); // = no white balance
    }

    imgsrc->getImage(currWB, tr, im, pp, ppar.toneCurve, ppar.raw);
    ImProcFunctions ipf(&ppar, true);

    if (ipf.needsTransform(fW, fH, imgsrc->getRotateDegree(), imgsrc->getMetaData())) {
        Imagefloat* trImg = new Imagefloat(fW, fH);
        ipf.transform(im, trImg, 0, 0, 0, 0, fW, fH, fW, fH,
                      imgsrc->getMetaData(), imgsrc->getRotateDegree(), true);
        delete im;
        im = trImg;
    }

    if (validParams->crop.enabled) {
        Imagefloat *tmpim = new Imagefloat(validParams->crop.w, validParams->crop.h);
        int cx = validParams->crop.x;
        int cy = validParams->crop.y;
        int cw = validParams->crop.w;
        int ch = validParams->crop.h;
#ifdef _OPENMP
        #pragma omp parallel for
#endif

        for (int i = cy; i < cy + ch; i++) {
            for (int j = cx; j < cx + cw; j++) {
                tmpim->r(i - cy, j - cx) = im->r(i, j);
                tmpim->g(i - cy, j - cx) = im->g(i, j);
                tmpim->b(i - cy, j - cx) = im->b(i, j);
            }
        }

        delete im;
        im = tmpim;
    }

    // image may contain out of range samples, clip them to avoid wrap-arounds
#ifdef _OPENMP
    #pragma omp parallel for
#endif

    for (int i = 0; i < im->getHeight(); i++) {
        for (int j = 0; j < im->getWidth(); j++) {
            im->r(i, j) = CLIP(im->r(i, j));
            im->g(i, j) = CLIP(im->g(i, j));
            im->b(i, j) = CLIP(im->b(i, j));
        }
    }

    int imw, imh;
    double tmpScale = ipf.resizeScale(validParams.get(), fW, fH, imw, imh);

    if (tmpScale != 1.0) {
        Imagefloat* tempImage = new Imagefloat(imw, imh);
        ipf.resize(im, tempImage, tmpScale);
        delete im;
        im = tempImage;
    }

    im->setMetadata(Exiv2Metadata(imgsrc->getFileName(), false));

    im->saveTIFF(fname, 16, false, true);
    delete im;

    if (plistener) {
        plistener->setProgressState(false);
    }

    //im->saveJPEG (fname, 85);
}

void ImProcCoordinator::stopProcessing()
{

    updaterThreadStart.lock();

    // Teardown or an image/scale transition invalidates the viewport state, so
    // any deferred analysis is dropped rather than run against soon-to-be
    // rebuilt buffers. The watcher must not flush while processing is stopped.
    paramsUpdateMutex.lock();
    deferredWatcherPaused = true;
    changeSinceLast = 0;
    pendingDeferredMask = 0;
    pendingDeferredHits = 0;
    pendingDeferredFirstUs = 0;
    pendingDeferredDeadlineUs = 0;
    paramsUpdateMutex.unlock();

    if (updaterRunning && thread) {
        thread->join();
    }

    invalidateViewportState();
    updaterThreadStart.unlock();
}

void ImProcCoordinator::startProcessing()
{
    updaterThreadStart.lock();

    if (!destroying && !updaterRunning) {
        updaterRunning = true;

        // Create the thread while holding updaterThreadStart. The GUI request
        // path and the deferred watcher can now call this concurrently; with
        // the check outside the lock both could create an updater thread, and
        // the second would overwrite `thread`, leaving the first unjoinable.
        thread = Glib::Thread::create(
            sigc::mem_fun(*this, &ImProcCoordinator::process), 0, true, true,
            Glib::THREAD_PRIORITY_NORMAL);
    }

    updaterThreadStart.unlock();
}

void ImProcCoordinator::startProcessing(int changeCode)
{
    paramsUpdateMutex.lock();
    deferredWatcherPaused = false;
    changeSinceLast |= changeCode;

    if (changeCode & ~M_VOID) {
        previewRequest.fetch_add(1, std::memory_order_relaxed);
    }

    paramsUpdateMutex.unlock();

    startProcessing();
}

void ImProcCoordinator::process()
{
    if (plistener) {
        plistener->setProgressState(true);
    }

    paramsUpdateMutex.lock();

    while (changeSinceLast) {
        const bool panningRelatedChange =
            params->toneCurve.isPanningRelatedChange(nextParams->toneCurve)
            || params->labCurve != nextParams->labCurve
            || params->locallab != nextParams->locallab
            || params->localContrast != nextParams->localContrast
            || params->rgbCurves != nextParams->rgbCurves
            || params->colorToning != nextParams->colorToning
            || params->vibrance != nextParams->vibrance
            //     || params->wb != nextParams->wb //isPanningRelatedChange(nextParams->wb)
            || params->wb.isPanningRelatedChange(nextParams->wb)
            || params->colorappearance != nextParams->colorappearance
            || params->epd != nextParams->epd
            || params->fattal != nextParams->fattal
            || params->sh != nextParams->sh
            || params->toneEqualizer != nextParams->toneEqualizer
            || params->crop != nextParams->crop
            || params->cropGuide != nextParams->cropGuide
            || params->coarse != nextParams->coarse
            || params->commonTrans != nextParams->commonTrans
            || params->rotate != nextParams->rotate
            || params->distortion != nextParams->distortion
            || params->lensProf != nextParams->lensProf
            || params->perspective != nextParams->perspective
            || params->gradient != nextParams->gradient
            || params->pcvignette != nextParams->pcvignette
            || params->cacorrection != nextParams->cacorrection
            || params->vignetting != nextParams->vignetting
            || params->chmixer != nextParams->chmixer
            || params->blackwhite != nextParams->blackwhite
            || params->icm != nextParams->icm
            || params->hsvequalizer != nextParams->hsvequalizer
            || params->filmSimulation != nextParams->filmSimulation
            || params->softlight != nextParams->softlight
            || params->raw != nextParams->raw
            || params->retinex != nextParams->retinex
            || params->wavelet != nextParams->wavelet
            || params->dirpyrequalizer != nextParams->dirpyrequalizer
            || params->dehaze != nextParams->dehaze
            || params->pdsharpening != nextParams->pdsharpening
            || params->filmNegative != nextParams->filmNegative
            || params->spot.enabled != nextParams->spot.enabled
            || sharpMaskChanged;

        sharpMaskChanged = false;
        *params = *nextParams;
        int change = changeSinceLast;
        changeSinceLast = 0;
        const unsigned request = previewRequest.load(std::memory_order_relaxed);

        if (tweakOperator) {
            // TWEAKING THE PROCPARAMS FOR THE SPOT ADJUSTMENT MODE
            backupParams();
            tweakOperator->tweakParams(*params);
        } else if (paramsBackup) {
            paramsBackup.release();
        }

        paramsUpdateMutex.unlock();

        // M_VOID means no update, and is a bit higher that the rest
        if (change & (~M_VOID)) {
            updatePreviewImage(change, panningRelatedChange, request);
        }

        paramsUpdateMutex.lock();
    }

    updaterRunning = false;
    paramsUpdateMutex.unlock();

    if (plistener) {
        plistener->setProgressState(false);
    }
}

ProcParams* ImProcCoordinator::beginUpdateParams()
{
    paramsUpdateMutex.lock();

    return nextParams.get();
}

void ImProcCoordinator::endUpdateParams(ProcEvent change)
{
    int action = RefreshMapper::getInstance()->getAction(change);
    endUpdateParams(action);
}

void ImProcCoordinator::endUpdateParams(int changeFlags)
{
    changeSinceLast |= changeFlags;
    paramsReady.store(true, std::memory_order_release);

    if (changeFlags & ~M_VOID) {
        previewRequest.fetch_add(1, std::memory_order_relaxed);
    }

    paramsUpdateMutex.unlock();
    startProcessing();
}

bool ImProcCoordinator::getHighQualComputed()
{
    // this function may only be called from detail windows
    if (!highQualityComputed) {
        if (App::get().options().prevdemo == PD_Sidecar) {
            // we already have high quality preview
            setHighQualComputed();
        } else {
            for (size_t i = 0; i < crops.size() - 1; ++i) { // -1, because last entry is the freshly created detail window
                if (crops[i]->get_skip() == 1) {   // there is at least one crop with skip == 1 => we already have high quality preview
                    setHighQualComputed();
                    break;
                }
            }
        }
    }

    return highQualityComputed;
}

void ImProcCoordinator::setHighQualComputed()
{
    highQualityComputed = true;
}

void ImProcCoordinator::setFastPreview(bool enabled)
{
    if (fastPreviewEnabled.exchange(enabled, std::memory_order_acq_rel)
            == enabled) {
        return;
    }

    if (!paramsReady.load(std::memory_order_acquire)
            || !mainPreviewFullResolution.load(std::memory_order_acquire)
            || !fastPreviewAllowed()) {
        return;
    }

    queueFullPreviewRefresh();
}

void ImProcCoordinator::setMainPreviewFullResolution(bool full)
{
    const bool wasFull = mainPreviewFullResolution.exchange(
        full, std::memory_order_acq_rel);

    if (wasFull == full) {
        return;
    }

    if (!paramsReady.load(std::memory_order_acquire)) {
        return;
    }

    const bool prevdemoFull =
        App::get().options().prevdemo == PD_Sidecar;
    const bool fastPreview =
        fastPreviewEnabled.load(std::memory_order_acquire)
        && fastPreviewAllowed();
    const PrevdemoBase basePrevdemo =
        lastPassPrevdemoBase.load(std::memory_order_acquire);
    const bool prevdemoKnown = basePrevdemo != PrevdemoBase::UNKNOWN;
    const bool prevdemoUsedForBase = basePrevdemo == PrevdemoBase::SIDECAR;

    if (!mainPreviewCrossingNeedsRefresh(prevdemoFull, fastPreview,
                                         wasFull, full, prevdemoKnown,
                                         prevdemoUsedForBase)) {
        return;
    }

    queueFullPreviewRefresh();
}

void ImProcCoordinator::queueFullPreviewRefresh()
{
    invalidateViewportState();

    paramsUpdateMutex.lock();
    changeSinceLast |= ALL | M_MONITOR;
    previewRequest.fetch_add(1, std::memory_order_relaxed);
    paramsUpdateMutex.unlock();

    startProcessing();
}

void ImProcCoordinator::requestUpdateWaveform()
{
    if (!hListener) {
        return;
    }

    bool updated = updateWaveforms();

    if (updated) {
        notifyHistogramChanged();
    }
}

void ImProcCoordinator::requestUpdateHistogram()
{
    if (!hListener) {
        return;
    }

    bool updated = updateLRGBHistograms();

    if (updated) {
        notifyHistogramChanged();
    }
}

void ImProcCoordinator::requestUpdateHistogramRaw()
{
    if (!hListener) {
        return;
    }

    // Don't need to actually update histogram because it is always
    // up-to-date.
    if (hist_raw_dirty) {
        hist_raw_dirty = false;
        notifyHistogramChanged();
    }
}

void ImProcCoordinator::requestUpdateVectorscopeHC()
{
    if (!hListener) {
        return;
    }

    bool updated = updateVectorscopeHC();

    if (updated) {
        notifyHistogramChanged();
    }
}

void ImProcCoordinator::requestUpdateVectorscopeHS()
{
    if (!hListener) {
        return;
    }

    bool updated = updateVectorscopeHS();

    if (updated) {
        notifyHistogramChanged();
    }
}

}
