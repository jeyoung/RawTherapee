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
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "array2D.h"
#include "colortemp.h"
#include "curves.h"
#include "dcrop.h"
#include "imagesource.h"
#include "improcfun.h"
#include "LUT.h"
#include "rtengine.h"
#include "stage/stage_cache_integration.h"
#include "stage/stage_dirty_blocks.h"
#include "stage/stage_render_state.h"

#include "rtgui/threadutils.h"

namespace Glib
{
class Thread;
}

namespace rtengine
{

using namespace procparams;

class Crop;
class Image8;
class PreviewBaseCache;
class TweakOperator;

/*
 * Opt-in preview dump hook (RT_PREVIEW_DUMP).
 *
 * Writes the full-frame preview and working image, and the visible crop, as
 * uncompressed 16-bit TIFF with a small JSON descriptor. The descriptor
 * carries only generated file names and image geometry: never the source
 * path, the dump directory or other environment-derived strings.
 */
namespace preview_dump
{

bool enabled();
void dump_full_frame(const Image8* preview, const Image8* work, int scale,
                     int todo);
void dump_crop(const Image8* crop, const char* tag, int visibleX,
               int visibleY, int visibleWidth, int visibleHeight, int skip);

} // namespace preview_dump

/** @brief Manages the image processing, espc. of the preview windows
  *
  * There is one ImProcCoordinator per edit panel.
  *
  * The ImProcCoordinator handle an sized down image representation of the full image, that is used when paning
  * and in the Navigator object.
  *
  * Each ImProcCoordinator handles an rtengine::Crop list, which process images too with their own pipeline,
  * but using this class' LUT and other precomputed parameters. The main preview area is displaying a non framed Crop object,
  * while detail windows are framed Crop objects.
  */
class ImProcCoordinator final : public StagedImageProcessor, public HistogramObservable
{

    friend class Crop;

protected:
    Imagefloat *orig_prev;
    Imagefloat *oprevi;
    Imagefloat *spotprev;
    LabImage *oprevl;
    LabImage *nprevl;
    Imagefloat *fattal_11_dcrop_cache; // global cache for ToneMapFattal02 used in 1:1 detail windows (except when denoise is active)
    Image8 *previmg;  // displayed image in monitor color space, showing the output profile as well (soft-proofing enabled, which then correspond to workimg) or not
    Image8 *workimg;  // internal image in output color space for analysis
    CieImage *ncie;

    ImageSource* imgsrc;

    ColorTemp currWB;
    ColorTemp currWBcust;
    ColorTemp autoWB;
    ColorTemp currWBloc;
    ColorTemp autoWBloc;

    double lastAwbEqual;
    StandardObserver lastAwbObserver{ColorTemp::DEFAULT_OBSERVER};
    double lastAwbTempBias;
    Glib::ustring lastAwbauto;

    Glib::ustring monitorProfile;
    RenderingIntent monitorIntent;
    bool softProof;
    bool gamutCheck;
    bool sharpMask;
    bool sharpMaskChanged;
    int scale;
    bool highDetailPreprocessComputed;
    bool highDetailRawComputed;
    bool highDetailStateKnown;
    bool lastHighDetailNeeded;

    // Per-editor-tab FAST preview state for 100% and above, plus the
    // explicitly tracked main-preview resolution. Crop geometry is updated
    // asynchronously, so it cannot be used to infer this at request time.
    std::atomic<bool> fastPreviewEnabled;
    std::atomic<bool> mainPreviewFullResolution;
    std::atomic<bool> paramsReady;

    // Preview Demosaic Method in force when the preview base was last built or
    // restored from the cache. The zoom-crossing guard must not compare
    // decisions derived from a preference the base was not built with.
    enum class PrevdemoBase { UNKNOWN, FAST, SIDECAR };
    std::atomic<PrevdemoBase> lastPassPrevdemoBase;

    // Viewport-aware preview fast path (disable with RT_VIEWPORT_FAST=0).
    // The snapshot describes the state produced by the last completed
    // full-frame pass, so the parent LUTs can be rebuilt without redoing the
    // base image or the full-frame analysis.
    std::atomic<bool> viewportStateValid;
    std::atomic<bool> viewportHighDetail;
    std::atomic<bool> forceFullPreview;
    std::atomic<unsigned> viewportDeferredRequest;
    std::atomic<bool> deferredCropRendered;
    ProcParams viewportFullParams;
    ProcParams viewportLutParams;
    ProcParams viewportDeferredParams;
    LUTu viewportZeroLhist;

    // Experimental, default-off cache of the preview base image returned by
    // ImageSource::getImage(). One snapshot only; deep copies in and out.
    std::unique_ptr<PreviewBaseCache> previewBaseCache;

    /*
     * Opt-in node-output cache for the preview raw.base tier (RT_NODE_CACHE).
     *
     * The tier is the `orig_prev` pixel buffer plus the small state the same
     * node derives from it (white balance, histogram, denoise validity). The
     * key is the exact `NodeCacheKey` of the pass. A hit substitutes for the
     * rebuild body only when the typed work order names no raw source producer
     * and the base rebuild itself is scheduled, so every downstream node
     * reruns from the restored tier. The byte budget and the switch are fixed
     * from the environment at construction.
     */
    stage::NodeCache nodeCache;

    /*
     * Input of the last completed full-frame preview pass, used to derive the
     * stage graph's dirty blocks without the event mask (devnotes/perf/
     * STAGE_RUNTIME.md). The parameter snapshot holds one fingerprint per
     * canonical preview descriptor and is taken before the pass, because the
     * pass normalises white balance and similar values in place. The render
     * state is taken after the pass, because its high-detail fields record the
     * work the pass completed. Both are invalidated whenever the buffers they
     * describe are reallocated or the source is replaced.
     */
    stage::ParamSnapshot previewDirtyFingerprints;
    stage::RenderStateKey previewDirtyState;
    bool previewDirtyFingerprintsValid = false;
    bool previewDirtyStateValid = false;

    /*
     * History of the last completed full-frame pass for the read-only
     * work-order shadow comparison. Both halves are snapshotted after the
     * crop render, because the crop analysis writes automatic denoise
     * strengths back into the live parameters (and the preview bodies write
     * white balance, while the GUI listeners write automatic black, contrast
     * and radius). Those corrections are consumed by the pass and must not
     * read as a new delta. Taking the fingerprint before the pass and the
     * parameters after it mixed two vintages and produced a false typed-only
     * run. Only maintained while RT_WORK_SHADOW is set.
     */
    stage::ParamSnapshot previewWorkFingerprints;
    procparams::ProcParams previewDirtyParams;
    bool previewDirtyParamsValid = false;

    // True while the initial rough preview is being rendered. Rough previews
    // must not populate the preview base cache.
    bool initialRoughPreview = false;

    bool allocated;

    void freeAll();

    // Precomputed values used by DetailedCrop ----------------------------------------------

    float bwAutoR, bwAutoG, bwAutoB;
    float CAMMean;
    LUTf hltonecurve;
    LUTf shtonecurve;
    LUTf tonecurve;

    LUTf lumacurve;
    LUTf chroma_acurve;
    LUTf chroma_bcurve;
    LUTf satcurve;
    LUTf lhskcurve;
    LUTf clcurve;
//    multi_array2D<float, 3> conversionBuffer;
    multi_array2D<float, 4> conversionBuffer;
    LUTf wavclCurve;
    LUTf clToningcurve;
    LUTf cl2Toningcurve;
    LUTf Noisecurve;
    LUTf NoiseCCcurve;

    LUTu vhist16, vhist16bw;
    LUTu lhist16CAM;
    LUTu lhist16CCAM;
    LUTu lhist16RETI;
    LUTu lhist16CLlad, lhist16LClad;
    LUTu histRed, histRedRaw;
    LUTu histGreen, histGreenRaw;
    LUTu histBlue, histBlueRaw;
    LUTu histLuma, histToneCurve, histToneCurveBW, histLCurve, histCCurve;
    LUTu histLLCurve, histLCAM, histCCAM, histClad, bcabhist, histChroma, histLRETI;
    bool hist_lrgb_dirty;
    /// Used to simulate a lazy update of the raw histogram.
    bool hist_raw_dirty;
    int vectorscopeScale;
    bool vectorscope_hc_dirty, vectorscope_hs_dirty;
    array2D<int> vectorscope_hc, vectorscope_hs;
    /// Waveform's intensity. Same as height of reference image.
    int waveformScale;
    bool waveform_dirty;
    array2D<int> waveformRed, waveformGreen, waveformBlue, waveformLuma;

    LUTf CAMBrightCurveJ, CAMBrightCurveQ;

    LUTf rCurve;
    LUTf gCurve;
    LUTf bCurve;
    ToneCurve customToneCurve1;
    ToneCurve customToneCurve2;
    ColorGradientCurve ctColorCurve;
    OpacityCurve ctOpacityCurve;
    NoiseCurve noiseLCurve;
    NoiseCurve noiseCCurve;
    WavCurve wavCLVCurve;
    WavCurve wavdenoise;
    WavCurve wavdenoiseh;
    Wavblcurve wavblcurve;
    WavOpacityCurveRG waOpacityCurveRG;
    WavOpacityCurveSH waOpacityCurveSH;
    WavOpacityCurveBY waOpacityCurveBY;
    WavOpacityCurveW waOpacityCurveW;
    WavOpacityCurveWL waOpacityCurveWL;
    WavOpacityCurveWL icmOpacityCurveWL;
    RetinextransmissionCurve dehatransmissionCurve;
    RetinexgaintransmissionCurve dehagaintransmissionCurve;

    ColorAppearance customColCurve1;
    ColorAppearance customColCurvered;
    ColorAppearance customColCurvegreen;
    ColorAppearance customColCurveblue;
    ColorAppearance customColCurve2;
    ColorAppearance customColCurve3;
    ToneCurve beforeToneCurveBW;
    ToneCurve afterToneCurveBW;

    LUTu rcurvehist, rcurvehistCropped, rbeforehist;
    LUTu gcurvehist, gcurvehistCropped, gbeforehist;
    LUTu bcurvehist, bcurvehistCropped, bbeforehist;

    // ------------------------------------------------------------------------------------

    int fw, fh, tr, fullw, fullh;
    int pW, pH;

    ProgressListener* plistener;
    PreviewImageListener* imageListener;
    AutoExpListener* aeListener;
    AutoCamListener* acListener;
    AutoBlackListener* ablListener;
    AutoBlackxListener* ablxListener;
    AutoBWListener* abwListener;
    AutoWBListener* awbListener;
    FlatFieldAutoClipListener *flatFieldAutoClipListener;
    AutoContrastListener *bayerAutoContrastListener;
    AutoContrastListener *xtransAutoContrastListener;
    AutoContrastListener *pdSharpenAutoContrastListener;
    AutoRadiusListener *pdSharpenAutoRadiusListener;
    FrameCountListener *frameCountListener;
    ImageTypeListener *imageTypeListener;
    FilmNegListener *filmNegListener;
    AutoColorTonListener* actListener;
    AutoprimListener* primListener;
    AutoChromaListener* adnListener;
    WaveletListener* awavListener;
    RetinexListener* dehaListener;
    CompgamutListener* acmaxListener;
  
//    LocallabListener* locallListener;

    
    HistogramListener* hListener;
    std::vector<SizeListener*> sizeListeners;

    std::vector<Crop*> crops;

    bool resultValid;

    MyMutex minit;  // to gain mutually exclusive access to ... to what exactly?

    void backupParams();
    void restoreParams();
    void allocCache (Imagefloat* &imgfloat);
    void notifyHistogramChanged();
    void reallocAll();
    /// Updates L, R, G, and B histograms. Returns true unless not updated.
    bool updateLRGBHistograms();
    /// Updates the H-C vectorscope. Returns true unless not updated.
    bool updateVectorscopeHC();
    /// Updates the H-S vectorscope. Returns true unless not updated.
    bool updateVectorscopeHS();
    /// Updates all waveforms. Returns true unless not updated.
    bool updateWaveforms();
    void setScale(int prevscale, bool allowSmall = false);
    void updatePreviewImage (int todo, bool panningRelatedChange, unsigned request);
    bool canUseViewportFastPath(int todo, bool highDetailNeeded,
                                const char*& reason) const;
    void prepareViewportLuts(int todo);
    void updateViewportLutSnapshot(int todo);
    void renderCrops(int todo, bool force);
    int queueViewportDeferredAnalysis(int todo, unsigned request);
    void markViewportStateValid(bool highDetailNeeded);
    void invalidateViewportState();
    void ensureDeferredWatcher();
    void deferredWatcherLoop();
    void flushDeferredAnalysis();
    void dropPendingDeferred();
    void absorbPendingDeferred(int& todo);

    /*
     * Canonical full-frame preview nodes (devnotes/perf/STAGE_PIPELINE.md).
     *
     * Each block of the full-frame preview is one method, and the stage graph
     * executes the plan's order through them. The per-pass state and the
     * resolved node plan are defined in improccoordinator.cc.
     */
    struct PreviewPassState;
    struct PreviewPassPlan;

    void stageSourceSelect(PreviewPassState& pass);
    void stageRawPreprocess(PreviewPassState& pass);
    void stageRawDemosaic(PreviewPassState& pass);
    void stageRawBase(PreviewPassState& pass);
    void stageTransform(PreviewPassState& pass);
    void stageLocalAdjustments(PreviewPassState& pass);
    void stageRgbProc(PreviewPassState& pass);
    void stageLabTools(PreviewPassState& pass);
    void stageLabWavelet(PreviewPassState& pass);
    void stageWorkingTrc(PreviewPassState& pass);
    void stageCieCam(PreviewPassState& pass);
    void stageGamut(PreviewPassState& pass);
    void stageMonitorOutput(PreviewPassState& pass);

    PreviewPassPlan planPreviewPass(PreviewPassState& pass);
    void executePreviewNode(const std::string& id, PreviewPassState& pass);

    /*
     * Node-output cache of the preview raw.base tier (RT_NODE_CACHE).
     *
     * nodeCacheRawBaseKey() names the tier from the pass state.
     * nodeCacheRestoreRawBase() substitutes a hit for the rebuild body and
     * returns false when the entry cannot be delivered. nodeCacheStoreRawBase()
     * records the tier at the end of stageRawBase, because only then are the
     * pixels, the white balance and the histogram of this pass final. Both are
     * no-ops when the switch is off.
     */
    stage::NodeCacheKey nodeCacheRawBaseKey(const PreviewPassState& pass) const;
    bool nodeCacheRestoreRawBase(const PreviewPassState& pass,
                                 const stage::NodeCacheKey& key);
    void nodeCacheStoreRawBase(const PreviewPassState& pass);

    /*
     * Dirty-block derivation for the graph scheduler.
     *
     * previewRenderState() snapshots the render state the pass is about to
     * run with; previewDirtyBlocks() merges the parameter-fingerprint and
     * render-state differences against the last completed full-frame pass.
     * recordPreviewDirtyState() stores the input of the pass that just
     * completed, and invalidatePreviewDirtyState() drops it when the buffers
     * it describes are gone.
     */
    stage::RenderStateKey previewRenderState(const PreviewPassState& pass) const;
    stage::DirtyBlocks previewDirtyBlocks(const PreviewPassState& pass) const;

    /*
     * Deferred analysis mask queued for the viewport fast path.
     *
     * Lock-free and safe to call on the render path: it does not take
     * paramsUpdateMutex, so building a RenderStateKey never blocks a request
     * update. That mutex also guards the viewport fast-path state, so this
     * accessor must read nothing but the atomic below. Every mutation of the
     * mask still happens under the mutex, so the burst bookkeeping stays
     * coherent. The load is relaxed because the mask carries no other data: the
     * returned value is an advisory snapshot that a concurrent fast-path hit
     * may change immediately after it is read, and nothing may treat it as a
     * claim about a later pass.
     */
    int32_t deferredAnalysisMask() const
    {
        return static_cast<int32_t>(
            pendingDeferredMask.load(std::memory_order_relaxed));
    }

    void recordPreviewDirtyState(const PreviewPassState& pass);
    void recordPreviewWorkHistory();
    void invalidatePreviewDirtyState();

    MyMutex mProcessing;
    const std::unique_ptr<ProcParams> params;  // used for the rendering, can be eventually tweaked
    std::unique_ptr<ProcParams> paramsBackup;  // backup of the untweaked procparams
    TweakOperator* tweakOperator;

    // for optimization purpose, the output profile, output rendering intent and
    // output BPC will trigger a regeneration of the profile on parameter change only
    // and automatically
    Glib::ustring lastOutputProfile;
    RenderingIntent lastOutputIntent;
    bool lastOutputBPC;

    // members of the updater:
    Glib::Thread* thread;
    MyMutex updaterThreadStart;
    MyMutex paramsUpdateMutex;
    int  changeSinceLast;
    std::atomic<unsigned> previewRequest{0};
    bool updaterRunning;
    const std::unique_ptr<ProcParams> nextParams;
    bool destroying;

    // Deferred full-frame analysis, see rtengine/viewportdefer.h.
    //
    // A viewport fast-path hit serves the crop immediately and accumulates the
    // analysis stages it skipped. The mask is run once the interaction settles,
    // so a drag costs one analysis pass instead of one per update. The fields
    // below are guarded by paramsUpdateMutex unless they are atomic.
    //
    // The mask itself is atomic so the render path can read it without taking
    // paramsUpdateMutex (see deferredAnalysisMask()); every mutation still
    // happens under that mutex, so there is still one source of truth.
    std::atomic<int> pendingDeferredMask;
    int pendingDeferredHits;
    gint64 pendingDeferredFirstUs;
    gint64 pendingDeferredDeadlineUs;
    bool deferredWatcherStarted;
    bool deferredWatcherPaused;
    std::atomic<bool> deferredWatcherStop;
    Glib::Thread* deferredWatcher;
    std::atomic<bool> deferredCropReusable;
    bool utili;
    bool autili;
    bool butili;
    bool ccutili;
    bool cclutili;
    bool clcutili;
    bool opautili;
    bool wavcontlutili;
    void startProcessing();
    void process();
    void queueFullPreviewRefresh();
    float colourToningSatLimit;
    float colourToningSatLimitOpacity;
    bool highQualityComputed;
    cmsHTRANSFORM customTransformIn;
    cmsHTRANSFORM customTransformOut;
    ImProcFunctions ipf;
    
    //locallab
    LocallabListener* locallListener;
    LUTf lllocalcurve;
    LUTf cllocalcurve;
    LUTf lclocalcurve;
    LUTf cclocalcurve;
    LUTf rgblocalcurve;
    LUTf exlocalcurve;
    LUTf hltonecurveloc;
    LUTf shtonecurveloc;
    LUTf tonecurveloc;
    LUTf lightCurveloc;
    LUTf lmasklocalcurve;
    LUTf lmaskexplocalcurve;
    LUTf lmaskSHlocalcurve;
    LUTf ghslocalcurve;
    LUTf lmaskviblocalcurve;
    LUTf lmasktmlocalcurve;
    LUTf lmaskretilocalcurve;
    LUTf lmaskcblocalcurve;
    LUTf lmaskbllocalcurve;
    LUTf lmasklclocalcurve;
    LUTf lmaskloglocalcurve;
    LUTf lmasklocal_curve;
    LUTf lmaskcielocalcurve;
    LUTf cielocalcurve;
    LUTf cielocalcurve2;
    LUTf jzlocalcurve;
    LUTf czlocalcurve;
    LUTf czjzlocalcurve;
    LUTf redlocalcurve;
    LUTf greenlocalcurve;
    LUTf bluelocalcurve;
    
    LocretigainCurve locRETgainCurve;
    LocretitransCurve locRETtransCurve;
    LocretigainCurverab locRETgainCurverab;
    LocLHCurve loclhCurve;
    LocHHCurve lochhCurve;
    LocCHCurve locchCurve;
    LocHHCurve lochhCurvejz;
    LocCHCurve locchCurvejz;
    LocLHCurve loclhCurvejz;
    LocCCmaskCurve locccmasCurve;
    LocLLmaskCurve locllmasCurve;
    LocHHmaskCurve lochhmasCurve;
    LocHHmaskCurve lochhhmasCurve;
    LocHHmaskCurve lochhhmascieCurve;
    LocCCmaskCurve locccmasexpCurve;
    LocLLmaskCurve locllmasexpCurve;
    LocHHmaskCurve lochhmasexpCurve;
    LocCCmaskCurve locccmasSHCurve;
    LocLLmaskCurve locllmasSHCurve;
    LocHHmaskCurve lochhmasSHCurve;
    LocCCmaskCurve locccmasvibCurve;
    LocLLmaskCurve locllmasvibCurve;
    LocHHmaskCurve lochhmasvibCurve;
    LocCCmaskCurve locccmaslcCurve;
    LocLLmaskCurve locllmaslcCurve;
    LocHHmaskCurve lochhmaslcCurve;
    LocCCmaskCurve locccmascbCurve;
    LocLLmaskCurve locllmascbCurve;
    LocHHmaskCurve lochhmascbCurve;
    LocCCmaskCurve locccmasretiCurve;
    LocLLmaskCurve locllmasretiCurve;
    LocHHmaskCurve lochhmasretiCurve;
    LocCCmaskCurve locccmastmCurve;
    LocLLmaskCurve locllmastmCurve;
    LocHHmaskCurve lochhmastmCurve;
    LocCCmaskCurve locccmasblCurve;
    LocLLmaskCurve locllmasblCurve;
    LocHHmaskCurve lochhmasblCurve;
    LocCCmaskCurve locccmas_Curve;
    LocLLmaskCurve locllmas_Curve;
    LocHHmaskCurve lochhmas_Curve;
    LocHHmaskCurve lochhhmas_Curve;
    LocCCmaskCurve locccmaslogCurve;
    LocLLmaskCurve locllmaslogCurve;
    LocHHmaskCurve lochhmaslogCurve;
    LocCCmaskCurve locccmascieCurve;
    LocLLmaskCurve locllmascieCurve;
    LocHHmaskCurve lochhmascieCurve;
    
    LocwavCurve locwavCurve;
    LocwavCurve loclmasCurveblwav;
    LocwavCurve loclmasCurvecolwav;
    LocwavCurve loclmasCurveciewav;
    LocwavCurve loclevwavCurve;
    LocwavCurve locconwavCurve;
    LocwavCurve loccompwavCurve;
    LocwavCurve loccomprewavCurve;
    LocwavCurve locwavCurveden;
    LocwavCurve locedgwavCurve;
    LocwavCurve loclmasCurve_wav;
    LocwavCurve locwavCurvehue;
    LocwavCurve locwavCurvehuecont;
    LocwavCurve locwavCurvejz;

    std::vector<float> huerefs;
    std::vector<float> huerefblurs;
    std::vector<float> chromarefblurs;
    std::vector<float> lumarefblurs;
    std::vector<float> chromarefs;
    std::vector<float> lumarefs;
    std::vector<float> sobelrefs;
    std::vector<float> avgs;
    std::vector<float> meantms;
    std::vector<float> stdtms;
    std::vector<float> meanretis;
    std::vector<float> stdretis;
    
    bool lastspotdup;
    bool previewDeltaE;
    int locallColorMask;
    int locallColorMaskinv;
    int locallExpMask;
    int locallExpMaskinv;
    int locallSHMask;
    int locallSHMaskinv;
    int locallvibMask;
    int localllcMask;
    int locallcbMask;
    int locallretiMask;
    int locallsoftMask;
    int localltmMask;
    int locallblMask;
    int locallsharMask;
    int localllogMask;
    int locall_Mask;
    int locallcieMask;

public:

    ImProcCoordinator ();
    ~ImProcCoordinator () override;
    void assign     (ImageSource* imgsrc);

    void        getParams (procparams::ProcParams* dst, bool tweaked=false) override;

    void        startProcessing (int changeCode) override;
    ProcParams* beginUpdateParams () override;
    void        endUpdateParams (ProcEvent change) override;  // must be called after beginUpdateParams, triggers update
    void        endUpdateParams (int changeFlags) override;
    void        stopProcessing () override;

    std::string *retistrsav;

    void setPreviewScale    (int scale) override
    {
        setPreviewScale(scale, false);
    }
    void setPreviewScale    (int scale, bool allowSmall) override
    {
        initialRoughPreview = allowSmall;
        setScale(scale, allowSmall);
    }
    int  getPreviewScale    () override
    {
        return scale;
    }

    //void fullUpdatePreviewImage  ();

    int getFullWidth () override
    {
        return fullw;
    }
    int getFullHeight () override
    {
        return fullh;
    }

    int getPreviewWidth () override
    {
        return pW;
    }
    int getPreviewHeight () override
    {
        return pH;
    }

    DetailedCrop* createCrop  (::EditDataProvider *editDataProvider, bool isDetailWindow) override;

    void setTweakOperator (TweakOperator *tOperator) override;
    void unsetTweakOperator (TweakOperator *tOperator) override;
    bool getAutoWB   (double& temp, double& green, double equal, StandardObserver observer, double tempBias) override;
    void getCamWB    (double& temp, double& green, StandardObserver observer) override;
    void getSpotWB   (int x, int y, int rectSize, double& temp, double& green) override;
    bool getFilmNegativeSpot(int x, int y, int spotSize, FilmNegativeParams::RGB &refInput, FilmNegativeParams::RGB &refOutput) override;
    void getAutoCrop (double ratio, int &x, int &y, int &w, int &h) override;
    bool getHighQualComputed() override;
    void setHighQualComputed() override;
    void setFastPreview(bool enabled) override;
    void setMainPreviewFullResolution(bool full) override;
    void setMonitorProfile (const Glib::ustring& profile, RenderingIntent intent) override;
    void getMonitorProfile (Glib::ustring& profile, RenderingIntent& intent) const override;
    void setSoftProofing   (bool softProof, bool gamutCheck) override;
    void getSoftProofing   (bool &softProof, bool &gamutCheck) override;
    ProcEvent setSharpMask (bool sharpMask) override;
    bool updateTryLock () override
    {
        return updaterThreadStart.trylock();
    }
    void updateUnLock () override
    {
        updaterThreadStart.unlock();
    }

    void setLocallabMaskVisibility(bool previewDeltaE, int locallColorMask, int locallColorMaskinv, int locallExpMask, int locallExpMaskinv, int locallSHMask, int locallSHMaskinv, int locallvibMask, int locallsoftMask, int locallblMask, int localltmMask, int locallretiMask, int locallsharMask, int localllcMask, int locallcbMask, int localllogMask, int locall_Mask, int locallcieMask) override
    {
        this->previewDeltaE = previewDeltaE;
        this->locallColorMask = locallColorMask;
        this->locallColorMaskinv = locallColorMaskinv;
        this->locallExpMask = locallExpMask;
        this->locallExpMaskinv = locallExpMaskinv;
        this->locallSHMask = locallSHMask;
        this->locallSHMaskinv = locallSHMaskinv;
        this->locallvibMask = locallvibMask;
        this->locallsoftMask = locallsoftMask;
        this->locallblMask = locallblMask;
        this->localltmMask = localltmMask;
        this->locallretiMask = locallretiMask;
        this->locallsharMask = locallsharMask;
        this->localllcMask = localllcMask;
        this->locallcbMask = locallcbMask;
        this->localllogMask = localllogMask;
        this->locall_Mask = locall_Mask;
        this->locallcieMask = locallcieMask;
    }

    void setProgressListener (ProgressListener* pl) override
    {
        plistener = pl;
    }
    void setPreviewImageListener    (PreviewImageListener* il) override
    {
        imageListener = il;
    }
    void setSizeListener     (SizeListener* il) override
    {
        sizeListeners.push_back(il);
    }
    void delSizeListener     (SizeListener* il) override
    {
        std::vector<SizeListener*>::iterator it = std::find(sizeListeners.begin(), sizeListeners.end(), il);

        if (it != sizeListeners.end()) {
            sizeListeners.erase(it);
        }
    }
    void setAutoExpListener  (AutoExpListener* ael) override
    {
        aeListener = ael;
    }
    void setHistogramListener (HistogramListener *h) override
    {
        if (hListener) {
            hListener->setObservable(nullptr);
        }
        hListener = h;
        if (h) {
            h->setObservable(this);
        }
    }
    void setAutoCamListener  (AutoCamListener* acl) override
    {
        acListener = acl;
    }
    void setAutoBlackListener  (AutoBlackListener* abl) override
    {
        ablListener = abl;
    }
    void setAutoBlackxListener  (AutoBlackxListener* ablx) override
    {
        ablxListener = ablx;
    }
    
    void setAutoBWListener   (AutoBWListener* abw) override
    {
        abwListener = abw;
    }
    void setAutoWBListener   (AutoWBListener* awb) override
    {
        awbListener = awb;
    }
    void setAutoColorTonListener   (AutoColorTonListener* bwct) override
    {
        actListener = bwct;
    }
    void setAutoprimListener   (AutoprimListener* pri) override
    {
        primListener = pri;
    }
    void setAutoChromaListener  (AutoChromaListener* adn) override
    {
        adnListener = adn;
    }
    void setCompgamutListener  (CompgamutListener* cop) override
    {
        acmaxListener = cop;
    }
    void setRetinexListener  (RetinexListener* adh) override
    {
        dehaListener = adh;
    }
    void setLocallabListener  (LocallabListener* lla) override
    {
        locallListener = lla;
    }
    void setWaveletListener  (WaveletListener* awa) override
    {
        awavListener = awa;
    }

    void setFrameCountListener  (FrameCountListener* fcl) override
    {
        frameCountListener = fcl;
    }

    void setFlatFieldAutoClipListener  (FlatFieldAutoClipListener* ffacl) override
    {
        flatFieldAutoClipListener = ffacl;
    }
    void setBayerAutoContrastListener  (AutoContrastListener* acl) override
    {
        bayerAutoContrastListener = acl;
    }

    void setXtransAutoContrastListener  (AutoContrastListener* acl) override
    {
        xtransAutoContrastListener = acl;
    }

    void setpdSharpenAutoRadiusListener  (AutoRadiusListener* acl) override
    {
        pdSharpenAutoRadiusListener = acl;
    }

    void setpdSharpenAutoContrastListener  (AutoContrastListener* acl) override
    {
        pdSharpenAutoContrastListener = acl;
    }

    void setImageTypeListener  (ImageTypeListener* itl) override
    {
        imageTypeListener = itl;
    }

    void setFilmNegListener  (FilmNegListener* fnl) override
    {
        filmNegListener = fnl;
    }

    void saveInputICCReference (const Glib::ustring& fname, bool apply_wb) override;

    InitialImage*  getInitialImage () override
    {
        return imgsrc;
    }

    cmsHTRANSFORM& getCustomTransformIn ()
    {
        return customTransformIn;
    }

    cmsHTRANSFORM& getCustomTransformOut ()
    {
        return customTransformOut;
    }

    struct DenoiseInfoStore {
        DenoiseInfoStore() : chM(0), max_r{}, max_b{}, ch_M{}, valid(false)  {}
        float chM;
        float max_r[9];
        float max_b[9];
        float ch_M[9];
        bool valid;

    } denoiseInfoStore;

    void requestUpdateHistogram() override;
    void requestUpdateHistogramRaw() override;
    void requestUpdateVectorscopeHC() override;
    void requestUpdateVectorscopeHS() override;
    void requestUpdateWaveform() override;
};

}
