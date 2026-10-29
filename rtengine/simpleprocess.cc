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

#include <glibmm/thread.h>
#include <glibmm/ustring.h>

#include <cstdlib>

#include "cieimage.h"
#include "clutstore.h"
#include "color.h"
#include "colortemp.h"
#include "curves.h"
#include "dcp.h"
#include "guidedfilter.h"
#include "iccstore.h"
#include "imagefloat.h"
#include "imagesource.h"
#include "improcfun.h"
#include "opencl/openclengine.h"
#include "labimage.h"
#include "metadata.h"
#include "mytime.h"
#include "processingjob.h"
#include "procparams.h"
#include "profiling.h"
#include "rawimagesource.h"
#include "rtengine.h"
#include "stage/algorithms/gamut_cl.h"
#include "stage/stage_buffer.h"
#include "stage/stage_graph.h"
#include "stage/stage_pipeline.h"
#include "stage/stage_profile.h"
#include "stage/lab_tools_context.h"
#include "stage/lab_wavelet_context.h"
#include "stage/local_adjustments_context.h"
#include "stage/stage_runtime.h"
#include "stage/working_trc_context.h"
#include "utils.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "rtgui/multilangmgr.h"
#include "rtgui/options.h"

namespace rtengine
{

namespace
{

template <typename T>
void adjust_radius(const T &default_param, double scale_factor, T &param)
{
    const double delta = (param - default_param) * scale_factor;
    param = default_param + delta;
}

/*
 * Convert one rectangle of a working-space RGB image to Lab.
 *
 * This is the per-pixel body of ImProcFunctions::rgb2lab(Imagefloat&,
 * LabImage&, workingSpace) restricted to a region. It exists so that the
 * early-resize stage can convert only the pixels a crop keeps instead of a
 * full-frame Lab image plus a copy out of it. Both paths fetch the same
 * working-space matrix and call the same Color helpers in the same order per
 * pixel, so the region result is bit-identical to converting the whole image
 * and copying the rectangle.
 *
 * @param src           Linear working-space RGB source.
 * @param dst           Lab destination; its size is the rectangle size.
 * @param x, y          Top-left corner of the rectangle in @p src, in pixels.
 * @param workingSpace  Working profile name.
 */
void rgbToLabRegion(const Imagefloat& src, LabImage& dst, int x, int y,
                    const Glib::ustring& workingSpace)
{
    assert(x >= 0 && y >= 0);
    assert(x + dst.W <= src.getWidth());
    assert(y + dst.H <= src.getHeight());

    const TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(workingSpace);
    const float wp[3][3] = {
        {static_cast<float>(wprof[0][0]), static_cast<float>(wprof[0][1]), static_cast<float>(wprof[0][2])},
        {static_cast<float>(wprof[1][0]), static_cast<float>(wprof[1][1]), static_cast<float>(wprof[1][2])},
        {static_cast<float>(wprof[2][0]), static_cast<float>(wprof[2][1]), static_cast<float>(wprof[2][2])}
    };

    const int W = dst.W;
    const int H = dst.H;

#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic,16)
#endif

    for (int i = 0; i < H; i++) {
        const int row = i + y;

        for (int j = 0; j < W; j++) {
            float X, Y, Z;
            Color::rgbxyz(src.r(row, j + x), src.g(row, j + x), src.b(row, j + x),
                          X, Y, Z, wp);
            Color::XYZ2Lab(X, Y, Z, dst.L[i][j], dst.a[i][j], dst.b[i][j]);
        }
    }
}

/*
 * Number of blocks in the canonical export inventory (devnotes/perf/
 * STAGE_PIPELINE.md). The executor below covers exactly this many nodes; a
 * mismatch aborts, because the graph is the only export sequence.
 */
constexpr size_t EXPORT_NODE_COUNT = 20;

/*
 * Export node ids the graph executor implements. buildPipelineSpec() names the
 * nodes; this list is the executor's side of the contract. The graph path
 * validates every planned node against it before executing anything, so an
 * inventory change that this file does not implement aborts instead of
 * silently skipping a block.
 */
const char* const EXPORT_NODE_IDS[] = {
    "source.select", "raw.preprocess", "raw.demosaic", "raw.base",
    "analysis", "transform", "local.adjustments", "rgb.proc", "lab.tools",
    "lab.sharpening", "lab.wavelet", "working.trc", "ciecam", "gamut",
    "crop", "resize", "output.sharpen", "output.convert", "framing",
    "output.icc"
};

static_assert(sizeof(EXPORT_NODE_IDS) / sizeof(EXPORT_NODE_IDS[0]) ==
                  EXPORT_NODE_COUNT,
              "the export executor must cover the canonical node inventory");

bool isExportNode(const std::string& id)
{
    for (const char* known : EXPORT_NODE_IDS) {
        if (id == known) {
            return true;
        }
    }
    return false;
}

/*
 * Report a plan this executor cannot run, and stop.
 *
 * The graph is the only export sequence now, so there is no legacy pass to
 * fall back to. A rejected plan means the inventory and this executor
 * disagree, which is a programming error: aborting keeps it visible instead of
 * exporting a partly processed image. The reason is reported once per process
 * so a batch logs it a single time before the abort.
 */
[[noreturn]] void abortStageGraph(const char* node, const char* reason)
{
    static std::mutex mutex;
    static std::set<std::string> logged;

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (logged.insert(std::string(reason) + ": " + node).second) {
            std::fprintf(stderr, "stage-graph: fatal: %s (%s)\n",
                         reason, node);
            std::fprintf(stderr,
                         "stage-graph: the graph is the only export sequence, "
                         "so there is nothing to fall back to\n");
        }
    }

    std::abort();
}

/* Report the accepted plan once per process, for operations and evidence. */
void logStageGraphActive(size_t nodes)
{
    static std::mutex mutex;
    static bool logged = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (logged) {
        return;
    }
    logged = true;

    std::fprintf(stderr,
                 "stage-graph: active, %zu nodes executed from the canonical "
                 "export inventory\n",
                 nodes);
}

/*
 * Run the end-of-process gamut node. The argument order is the one `run_gamut`
 * documents in the device backend: the Lab buffer in place, the four matrices,
 * the gain, the power and the compression flag.
 *
 * The matrices are derived by the shared helper, in double, exactly as
 * ImProcFunctions::gamutcompr() does for the end of the process, so the device
 * and the host block in stage_gamut() cannot disagree about the gamut mapping.
 */
bool run_gamut_stage(const procparams::ColorManagementParams& icm,
                     LabImage& lab)
{
    stage::GamutNodeParams params;
    std::string error;
    if (!stage::gamut_node_params(icm, params, error)) {
        return false;
    }

    stage::GamutNodeArgs node;
    stage::gamut_node_args(lab, params, node);

    RtStageProfiling profiling{};
    return stage::run("gamut", std::vector<RtStageArg>(node.args, node.args + 8),
                      &profiling) == RT_STAGE_OK;
}

class ImageProcessor
{
public:
    ImageProcessor(
        ProcessingJob* pjob,
        int& errorCode,
        ProgressListener* pl,
        bool flush
    ) :
        job(static_cast<ProcessingJobImpl*>(pjob)),
        errorCode(errorCode),
        pl(pl),
        flush(flush),
        // internal state
        initialImage(nullptr),
        imgsrc(nullptr),
        fw(0),
        fh(0),
        tr(0),
        pp(0, 0, 0, 0, 0),
        calclum(nullptr),
        autoNR(0.f),
        autoNRmax(0.f),
        tilesize(0),
        overlap(0),
        ch_M(nullptr),
        max_r(nullptr),
        max_b(nullptr),
        min_b(nullptr),
        min_r(nullptr),
        lumL(nullptr),
        chromC(nullptr),
        ry(nullptr),
        sk(nullptr),
        pcsk(nullptr),
        expcomp(0.0),
        bright(0),
        contr(0),
        black(0),
        hlcompr(0),
        hlcomprthresh(0),
        baseImg(nullptr),
        labView(nullptr),
        cx(0),
        cy(0),
        cw(0),
        ch(0),
        labResize(false),
        framingData(),
        readyImg(nullptr),
        ctColorCurve(),
        autili(false),
        butili(false)
    {
    }

    Imagefloat *operator()()
    {
        if (!job->fast) {
            return normal_pipeline();
        } else {
            return fast_pipeline();
        }
    }

private:
    Imagefloat *normal_pipeline()
    {
        RT_PROFILE("proc::normal_pipeline", PROCESS_IMAGE);

        return graph_pipeline();
    }

    /*
     * Graph-driven form of the export pipeline. The graph is built from the
     * canonical export inventory every job, so its 20 nodes, 19 edges and
     * reuse modes come from buildPipelineSpec() rather than from this file.
     * A fresh graph is fully dirty, so the plan must let every node recompute;
     * a plan that would skip a block is rejected before anything executes and
     * aborts, because there is no second sequence to run. Node ids double as
     * the RT_STAGE_PROFILE keys.
     */
    Imagefloat *graph_pipeline()
    {
        const stage::StagePipelineSpec spec =
            stage::buildPipelineSpec(stage::StagePath::Export);

        if (spec.nodes.size() != EXPORT_NODE_COUNT) {
            abortStageGraph("inventory", "unexpected node count");
        }

        stage::StageGraph graph;
        std::vector<size_t> node;
        node.reserve(spec.nodes.size());

        for (const auto& descriptor : spec.nodes) {
            node.push_back(graph.addNode(descriptor.id, descriptor.reuse));
        }

        for (const auto& edge : spec.edges) {
            size_t from = spec.nodes.size();
            size_t to = spec.nodes.size();

            for (size_t i = 0; i < spec.nodes.size(); ++i) {
                if (spec.nodes[i].id == edge.first) {
                    from = i;
                }

                if (spec.nodes[i].id == edge.second) {
                    to = i;
                }
            }

            assert(from < spec.nodes.size() && to < spec.nodes.size());
            graph.connect(node[from], node[to]);
        }

        std::vector<size_t> order;

        if (!graph.topologicalOrder(order) || order.size() != spec.nodes.size()) {
            abortStageGraph("inventory", "graph is not acyclic");
        }

        const std::vector<stage::StageDecision> plan = graph.plan();

        for (size_t index : order) {
            const std::string& id = graph.node(index).name;

            if (!isExportNode(id)) {
                abortStageGraph(id.c_str(), "node has no executor");
            }

            // ReuseInput still means the node runs (it writes in place); only
            // ReuseOutput skips a block. This path has no stage buffer cache,
            // so a skip would silently drop work the pipeline must do.
            if (plan[index].action == stage::StageAction::ReuseOutput) {
                abortStageGraph(id.c_str(), "plan would skip the node");
            }
        }

        for (size_t index : order) {
            if (!executeExportNode(graph.node(index).name)) {
                return nullptr;
            }
        }

        logStageGraphActive(order.size());

        return readyImg;
    }

    /*
     * Run one canonical export node. The ids come from buildPipelineSpec(); an
     * unknown id has already been rejected by graph_pipeline(), so this only
     * maps a planned node to the method that implements it.
     */
    bool executeExportNode(const std::string& id)
    {
        if (id == "source.select") {
            return stage_source_select();
        } else if (id == "raw.preprocess") {
            stage_raw_preprocess();
        } else if (id == "raw.demosaic") {
            stage_raw_demosaic();
        } else if (id == "raw.base") {
            stage_raw_base();
        } else if (id == "analysis") {
            stage_analysis();
        } else if (id == "transform") {
            stage_transform();
        } else if (id == "local.adjustments") {
            stage_local_adjustments();
        } else if (id == "rgb.proc") {
            stage_rgb_proc();
        } else if (id == "lab.tools") {
            stage_lab_tools();
        } else if (id == "lab.sharpening") {
            stage_lab_sharpening();
        } else if (id == "lab.wavelet") {
            stage_lab_wavelet();
        } else if (id == "working.trc") {
            stage_working_trc();
        } else if (id == "ciecam") {
            stage_ciecam();
        } else if (id == "gamut") {
            stage_gamut();
        } else if (id == "crop") {
            stage_crop();
        } else if (id == "resize") {
            stage_resize();
        } else if (id == "output.sharpen") {
            stage_output_sharpen();
        } else if (id == "output.convert") {
            stage_output_convert();
        } else if (id == "framing") {
            stage_framing();
        } else if (id == "output.icc") {
            return stage_output_icc() != nullptr;
        } else {
            assert(false);
            return false;
        }

        return true;
    }

    Imagefloat *fast_pipeline()
    {
        RT_PROFILE("proc::fast_pipeline", PROCESS_IMAGE);

        if (!job->pparams.resize.enabled) {
            return normal_pipeline();
        }

        pl = nullptr;

        // The fast path keeps its own order: the early resize runs after the
        // transform and before denoise, so it calls the same node methods in a
        // different sequence rather than through the export graph.
        if (!stage_source_select()) {
            return nullptr;
        }

        stage_raw_preprocess();
        stage_raw_demosaic();
        stage_raw_base_source();
        stage_raw_color();
        stage_analysis();
        stage_transform();
        stage_early_resize();
        stage_denoise();
        return stage_finish();
    }

    /*
     * source.select: load or take the initial image, resolve the raw source,
     * create the processing functions and clamp the crop rectangle to the
     * frame. Extracted from stage_init() so the graph node has a single
     * implementation.
     */
    bool stage_source_select()
    {
        stage::ScopedProfile node_profile("source.select");

        errorCode = 0;

        if (pl) {
            pl->setProgressStr("PROGRESSBAR_PROCESSING");
            pl->setProgress(0.0);
        }

        initialImage = job->initialImage;

        if (!initialImage) {
            initialImage = InitialImage::load(job->fname, job->isRaw, &errorCode);

            if (errorCode) {
                delete job;
                return false; //return nullptr;
            }
        }

        procparams::ProcParams& params = job->pparams;

        // acquire image from imagesource
        imgsrc = initialImage->getImageSource();

        tr = getCoarseBitMask(params.coarse);

        if (imgsrc->getSensorType() == ST_BAYER) {
            if (params.raw.bayersensor.method != RAWParams::BayerSensor::getMethodString(RAWParams::BayerSensor::Method::PIXELSHIFT)) {
                imgsrc->setBorder(params.raw.bayersensor.border);
            } else {
                imgsrc->setBorder(std::max(params.raw.bayersensor.border, 2));
            }
        } else if (imgsrc->getSensorType() == ST_FUJI_XTRANS) {
            imgsrc->setBorder(params.raw.xtranssensor.border);
        }

        imgsrc->getFullSize(fw, fh, tr);

        // check the crop params
        if (params.crop.x > fw || params.crop.y > fh) {
            // the crop is completely out of the image, so we disable the crop
            params.crop.enabled = false;
            // and we set the values to the defaults
            params.crop.x = 0;
            params.crop.y = 0;
            params.crop.w = fw;
            params.crop.h = fh;
        } else {
            if (params.crop.x < 0) {
                params.crop.x = 0;
            }

            if (params.crop.y < 0) {
                params.crop.y = 0;
            }

            if ((params.crop.x + params.crop.w) > fw) {
                // crop overflow in the width dimension ; we trim it
                params.crop.w = fw - params.crop.x;
            }

            if ((params.crop.y + params.crop.h) > fh) {
                // crop overflow in the height dimension ; we trim it
                params.crop.h = fh - params.crop.y;
            }
        }

//    MyTime t1,t2;
//    t1.set();

        ipf_p.reset(new ImProcFunctions(&params, true));
        ImProcFunctions &ipf = * (ipf_p.get());
        ipf.setOpenCLAllowed(App::get().options().rtSettings.useOpenCL);
        opencl::setStageAllowed(App::get().options().rtSettings.useOpenCL);
        /*
         * The graph device path follows the same user setting as the classic
         * export path. Export runs in both OpenCL modes ("export only" is mode
         * 0), so the batch policy is governed by useOpenCL alone; the preview
         * coordinator additionally requires mode 1.
         */
        stage::set_opencl_stage_allowed(
            App::get().options().rtSettings.useOpenCL);

        imgsrc->setCurrentFrame(params.raw.bayersensor.imageNum);
        return true;
    }

    /*
     * raw.preprocess: black/white level, hot and dead pixels, chromatic
     * aberration and flat field. Extracted from stage_init() unchanged.
     */
    void stage_raw_preprocess()
    {
        stage::ScopedProfile node_profile("raw.preprocess");

        procparams::ProcParams& params = job->pparams;

        float reddeha = 0.f;
        float greendeha = 0.f;
        float bluedeha = 0.f;
        imgsrc->preprocess(params.raw, params.lensProf, params.coarse, reddeha, greendeha, bluedeha, params.dirpyrDenoise.enabled);

        if (pl) {
            pl->setProgress(0.20);
        }
    }

    /*
     * raw.demosaic: demosaic and capture sharpening. Extracted from
     * stage_init() unchanged.
     */
    void stage_raw_demosaic()
    {
        stage::ScopedProfile node_profile("raw.demosaic");

        procparams::ProcParams& params = job->pparams;

        bool autoContrast = imgsrc->getSensorType() == ST_BAYER ? params.raw.bayersensor.dualDemosaicAutoContrast : params.raw.xtranssensor.dualDemosaicAutoContrast;
        double contrastThreshold = imgsrc->getSensorType() == ST_BAYER ? params.raw.bayersensor.dualDemosaicContrast : params.raw.xtranssensor.dualDemosaicContrast;

        imgsrc->demosaic(params.raw, autoContrast, contrastThreshold, params.pdsharpening.enabled && pl);

        if (params.pdsharpening.enabled) {
            imgsrc->captureSharpening(params.pdsharpening, false, params.pdsharpening.contrast, params.pdsharpening.deconvradius);
        }


        if (pl) {
            pl->setProgress(0.30);
        }
    }

    /*
     * raw.base, first part: white balance, denoise preparation, the base
     * image and the tone-curve analysis. Extracted from stage_init() unchanged.
     * raw.base also runs stage_denoise() and stage_raw_color(); this split
     * exists so the fast export can run denoise after its early resize.
     */
    void stage_raw_base_source()
    {
        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        pp = PreviewProps(0, 0, fw, fh, 1);

        if (params.retinex.enabled) { //enabled Retinex
            LUTf cdcurve(65536, 0);
            LUTf mapcurve(65536, 0);
            RetinextransmissionCurve dehatransmissionCurve;
            RetinexgaintransmissionCurve dehagaintransmissionCurve;
            bool dehacontlutili = false;
            bool mapcontlutili = false;
            bool useHsl = false;
            multi_array2D<float, 4> conversionBuffer(1, 1);
            imgsrc->retinexPrepareBuffers(params.icm, params.retinex, conversionBuffer, dummy);
            imgsrc->retinexPrepareCurves(params.retinex, cdcurve, mapcurve, dehatransmissionCurve, dehagaintransmissionCurve, dehacontlutili, mapcontlutili, useHsl, dummy, dummy);
            float minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax;
            imgsrc->retinex(params.icm, params.retinex, params.toneCurve, cdcurve, mapcurve, dehatransmissionCurve, dehagaintransmissionCurve, conversionBuffer, dehacontlutili, mapcontlutili, useHsl, minCD, maxCD, mini, maxi, Tmean, Tsigma, Tmin, Tmax, dummy);
        }

        if (pl) {
            pl->setProgress(0.40);
        }

        // imgsrc->HLRecovery_Global(params.toneCurve);


        if (pl) {
            pl->setProgress(0.45);
        }

        // set the color temperature
        currWB = ColorTemp(params.wb.temperature, params.wb.green, params.wb.equal, params.wb.method, params.wb.observer);
        ColorTemp currWBitc;

        if (params.wb.method == "autitcgreen") {
            imgsrc->getrgbloc(0, 0, fh, fw, 0, 0, fh, fw, params.wb);
        }
        const bool autowb = (params.wb.method == "autitcgreen" && imgsrc->isRAW());
        ColorTemp autoWB;
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

        if (!params.wb.enabled) {
            currWB = ColorTemp();
        } else if (params.wb.method == "Camera" || (params.wb.method == "autitcgreen" && params.wb.compat_version >= 2 && !imgsrc->isRAW() && flush)) {//Use also Camera settings for Temperature correlation and TIF/Jpg
            currWB = imgsrc->getWB();
        } else if (params.wb.method == "autold") {//for Auto RGB
            double rm, gm, bm;
            if (params.wb.compat_version == 1 && !imgsrc->isRAW()) {
                // RGB grey compatibility version 1 used the identity
                // multipliers plus temperature bias for non-raw files.
                rm = gm = bm = 1.;
            } else {
                imgsrc->getAutoWBMultipliers(rm, gm, bm);
            }
            currWB.update(rm, gm, bm, params.wb.equal, params.wb.observer, params.wb.tempBias);

        } else if (autowb) {//for auto Itcwb - flush to enable only when batch only with Raw files
        //code similar to that present in improccoordinator.cc
                double rm;
                double gm;
                double bm;
                imgsrc->getAutoWBMultipliersItcGreen(
                    params,
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
                    0,
                    0.,
                    false,
                    autoWB,
                    rm,
                    gm,
                    bm);

                currWB = autoWB;
        } else if (params.wb.method == "autitcgreen" && params.wb.compat_version == 1 && !imgsrc->isRAW() && flush) {
            // ITCWB compatibility version 1 used 5000 K and observer 10 degrees
            // for non-raw files.
            currWB = ColorTemp(5000., 1., 1., params.wb.method, StandardObserver::TEN_DEGREES);
            currWB.convertObserver(params.wb.observer);
            params.wb.temperature = currWB.getTemp();
            params.wb.green = currWB.getGreen();
            params.wb.equal = currWB.getEqual();
        }

        //end WB auto

        calclum = nullptr ;
        params.dirpyrDenoise.getCurves(noiseLCurve, noiseCCurve);
        autoNR = (float) settings->nrauto;//
        autoNRmax = (float) settings->nrautomax;//

        if (settings->leveldnti == 0) {
            tilesize = 1024;
            overlap = 128;
        }

        if (settings->leveldnti == 1) {
            tilesize = 768;
            overlap = 96;
        }

        //  const int tilesize = 768;
        //  const int overlap = 96;
        int numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip;
        ipf.Tile_calc(tilesize, overlap, 2, fw, fh, numtiles_W, numtiles_H, tilewidth, tileheight, tileWskip, tileHskip);
        int nbtl = numtiles_W * numtiles_H;

        if ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT") || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO")) {
            nbtl = 9;
        }

        ch_M = new float [nbtl];//allocate memory
        max_r = new float [nbtl];
        max_b = new float [nbtl];
        min_b = new float [9];
        min_r = new float [9];
        lumL = new float [nbtl];
        chromC = new float [nbtl];
        ry = new float [nbtl];
        sk = new float [nbtl];
        pcsk = new float [nbtl];

        //  printf("expert=%d\n",settings->leveldnautsimpl);
        if (settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "PON") {
            MyTime t1pone, t2pone;
            t1pone.set();
            int crW = 100; // settings->leveldnv == 0
            int crH = 100; // settings->leveldnv == 0

            if (settings->leveldnv == 1) {
                crW = 250;
                crH = 250;
            }

            if (settings->leveldnv == 2) {
                crW = int (tileWskip / 2);
                crH = int (tileHskip / 2);
            }

            //  if(settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int(1.15f*(tileWskip/2));}//adapted to scale of preview
            if (settings->leveldnv == 3) {
                crW = tileWskip - 10;
                crH = tileHskip - 10;
            }

            float lowdenoise = 1.f;
            int levaut = settings->leveldnaut;

            if (levaut == 1) { //Standard
                lowdenoise = 0.7f;
            }

            //  int crW=tileWskip-10;//crop noise width
            //  int crH=tileHskip-10;//crop noise height
//      Imagefloat *origCropPart;//init auto noise
//          origCropPart = new Imagefloat (crW, crH);//allocate memory
            if (params.dirpyrDenoise.enabled) {//evaluate Noise
                LUTf gamcurve(65536, 0);
                float gam, gamthresh, gamslope;
                ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
#ifdef _OPENMP
                #pragma omp parallel
#endif
                {
                    Imagefloat *origCropPart;//init auto noise
                    origCropPart = new Imagefloat(crW, crH); //allocate memory
                    Imagefloat *provicalc = new Imagefloat((crW + 1) / 2, (crH + 1) / 2);  //for denoise curves
                    int skipP = 1;
#ifdef _OPENMP
                    #pragma omp for schedule(dynamic) collapse(2) nowait
#endif

                    for (int wcr = 0; wcr < numtiles_W; wcr++) {
                        for (int hcr = 0; hcr < numtiles_H; hcr++) {
                            int beg_tileW = wcr * tileWskip + tileWskip / 2.f - crW / 2.f;
                            int beg_tileH = hcr * tileHskip + tileHskip / 2.f - crH / 2.f;
                            PreviewProps ppP(beg_tileW, beg_tileH, crW, crH, skipP);
                            imgsrc->getImage(currWB, tr, origCropPart, ppP, params.toneCurve, params.raw);
                            //baseImg->getStdImage(currWB, tr, origCropPart, ppP, true, params.toneCurve);

                            // we only need image reduced to 1/4 here
                            for (int ii = 0; ii < crH; ii += 2) {
                                for (int jj = 0; jj < crW; jj += 2) {
                                    provicalc->r(ii >> 1, jj >> 1) = origCropPart->r(ii, jj);
                                    provicalc->g(ii >> 1, jj >> 1) = origCropPart->g(ii, jj);
                                    provicalc->b(ii >> 1, jj >> 1) = origCropPart->b(ii, jj);
                                }
                            }

                            imgsrc->convertColorSpace(provicalc, params.icm, currWB);  //for denoise luminance curve
                            float maxr = 0.f;
                            float maxb = 0.f;
                            float pondcorrec = 1.0f;
                            float chaut, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc;
                            int Nb;
                            chaut = 0.f;
                            redaut = 0.f;
                            blueaut = 0.f;
                            maxredaut = 0.f;
                            maxblueaut = 0.f;
                            chromina = 0.f;
                            sigma = 0.f;
                            ipf.RGB_denoise_info(origCropPart, provicalc, imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope, params.dirpyrDenoise, imgsrc->getDirPyrDenoiseExpComp(), chaut, Nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc);
                            float multip = 1.f;
                            float adjustr = 1.f;

                            if (params.icm.workingProfile == "ProPhoto")   {
                                adjustr = 1.f;   //
                            } else if (params.icm.workingProfile == "Adobe RGB")  {
                                adjustr = 1.f / 1.3f;
                            } else if (params.icm.workingProfile == "sRGB")       {
                                adjustr = 1.f / 1.3f;
                            } else if (params.icm.workingProfile == "WideGamut")  {
                                adjustr = 1.f / 1.1f;
                            } else if (params.icm.workingProfile == "Rec2020")  {
                                adjustr = 1.f / 1.1f;
                            } else if (params.icm.workingProfile == "Beta RGB")   {
                                adjustr = 1.f / 1.2f;
                            } else if (params.icm.workingProfile == "BestRGB")    {
                                adjustr = 1.f / 1.2f;
                            } else if (params.icm.workingProfile == "BruceRGB")   {
                                adjustr = 1.f / 1.2f;
                            }

                            if (!imgsrc->isRAW()) {
                                multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good for gamma=1
                            }

                            float maxmax = max(maxredaut, maxblueaut);
                            float delta;
                            int mode = 2;
                            int lissage = settings->leveldnliss;
                            ipf.calcautodn_info(chaut, delta, Nb, levaut, maxmax, lumema, chromina, mode, lissage, redyel, skinc, nsknc);

                            //    printf("PROCESS cha=%f red=%f bl=%f redM=%f bluM=%f chrom=%f sigm=%f lum=%f sigL=%f\n",chaut,redaut,blueaut, maxredaut, maxblueaut, chromina, sigma, lumema, sigma_L);
                            if (maxredaut > maxblueaut) {
                                maxr = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                                if (minblueaut <= minredaut  && minblueaut < chaut) {
                                    maxb = (-chaut + minblueaut) / (autoNRmax * multip * adjustr * lowdenoise);
                                }
                            } else {
                                maxb = (delta) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);

                                if (minredaut <= minblueaut  && minredaut < chaut) {
                                    maxr = (-chaut + minredaut) / (autoNRmax * multip * adjustr * lowdenoise);
                                }
                            }//maxb mxr - empirical evaluation red / blue

                            ch_M[hcr * numtiles_W + wcr] = pondcorrec * chaut / (autoNR * multip * adjustr * lowdenoise);
                            max_r[hcr * numtiles_W + wcr] = pondcorrec * maxr;
                            max_b[hcr * numtiles_W + wcr] = pondcorrec * maxb;
                            lumL[hcr * numtiles_W + wcr] = lumema;
                            chromC[hcr * numtiles_W + wcr] = chromina;
                            ry[hcr * numtiles_W + wcr] = redyel;
                            sk[hcr * numtiles_W + wcr] = skinc;
                            pcsk[hcr * numtiles_W + wcr] = nsknc;

                        }
                    }

                    delete provicalc;
                    delete origCropPart;
                }

                int liss = settings->leveldnliss; //smooth result around mean

                if (liss == 2 || liss == 3) {
                    // I smooth only mean and not delta (max)
                    float nchm = 0.f;
                    float koef = 0.4f; //between 0.1 to 0.9

                    if (liss == 3) {
                        koef = 0.0f;    //quasi auto for mean Ch
                    }

                    for (int wcr = 0; wcr < numtiles_W; wcr++) {
                        for (int hcr = 0; hcr < numtiles_H; hcr++) {
                            nchm += ch_M[hcr * numtiles_W + wcr];
                        }
                    }

                    nchm /= (numtiles_H * numtiles_W);

                    for (int wcr = 0; wcr < numtiles_W; wcr++) {
                        for (int hcr = 0; hcr < numtiles_H; hcr++) {
                            ch_M[hcr * numtiles_W + wcr] = nchm + (ch_M[hcr * numtiles_W + wcr] - nchm) * koef;
                        }
                    }
                }

                if (liss == 3) { //same as auto but with much cells
                    float MaxR = 0.f;
                    float MaxB = 0.f;
                    float MaxRMoy = 0.f;
                    float MaxBMoy = 0.f;

                    for (int k = 0; k < nbtl; k++) {
                        MaxBMoy += max_b[k];
                        MaxRMoy += max_r[k];

                        if (max_r[k] > MaxR) {
                            MaxR = max_r[k];
                        }

                        if (max_b[k] > MaxB) {
                            MaxB = max_b[k];
                        }

                    }

                    MaxBMoy /= nbtl;
                    MaxRMoy /= nbtl;

                    for (int k = 0; k < nbtl; k++) {
                        if (MaxR > MaxB) {
                            max_r[k] = MaxRMoy + (MaxR - MaxRMoy) * 0.66f; //#std Dev
                            //max_b[k]=MinB;
                            max_b[k] = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;

                        } else {
                            max_b[k] = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;
                            //max_r[k]=MinR;
                            max_r[k] = MaxRMoy + (MaxR - MaxRMoy) * 0.66f;

                        }
                    }
                }

                if (settings->verbose) {
                    t2pone.set();
                    printf("Info denoise ponderated performed in %d usec:\n", t2pone.etime(t1pone));
                }

            }
        }


        if ((settings->leveldnautsimpl == 1 && params.dirpyrDenoise.Cmethod == "AUT")  || (settings->leveldnautsimpl == 0 && params.dirpyrDenoise.C2method == "AUTO")) {
            MyTime t1aue, t2aue;
            t1aue.set();
            int crW, crH;

            if (settings->leveldnv == 0) {
                crW = 100;
                crH = 100;
            }

            if (settings->leveldnv == 1) {
                crW = 250;
                crH = 250;
            }

            if (settings->leveldnv == 2) {
                crW = int (tileWskip / 2);
                crH = int (tileHskip / 2);
            }

            //  if(settings->leveldnv ==2) {crW=int(tileWskip/2);crH=int(1.15f*(tileWskip/2));}//adapted to scale of preview
            if (settings->leveldnv == 3) {
                crW = tileWskip - 10;
                crH = tileHskip - 10;
            }

            float lowdenoise = 1.f;
            int levaut = settings->leveldnaut;

            if (levaut == 1) { //Standard
                lowdenoise = 0.7f;
            }

            if (params.dirpyrDenoise.enabled) {//evaluate Noise
                LUTf gamcurve(65536, 0);
                float gam, gamthresh, gamslope;
                ipf.RGB_denoise_infoGamCurve(params.dirpyrDenoise, imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope);
                int Nb[9];
                int  coordW[3];//coordinate of part of image to measure noise
                int  coordH[3];
                int begW = 50;
                int begH = 50;
                coordW[0] = begW;
                coordW[1] = fw / 2 - crW / 2;
                coordW[2] = fw - crW - begW;
                coordH[0] = begH;
                coordH[1] = fh / 2 - crH / 2;
                coordH[2] = fh - crH - begH;
#ifdef _OPENMP
                #pragma omp parallel
#endif
                {
                    Imagefloat *origCropPart;//init auto noise
                    origCropPart = new Imagefloat(crW, crH); //allocate memory
                    origCropPart->fill(0.f);
                    Imagefloat *provicalc = new Imagefloat((crW + 1) / 2, (crH + 1) / 2);  //for denoise curves

#ifdef _OPENMP
                    #pragma omp for schedule(dynamic) collapse(2) nowait
#endif

                    for (int wcr = 0; wcr <= 2; wcr++) {
                        for (int hcr = 0; hcr <= 2; hcr++) {
                            PreviewProps ppP(coordW[wcr], coordH[hcr], crW, crH, 1);
                            imgsrc->getImage(currWB, tr, origCropPart, ppP, params.toneCurve, params.raw);
                            //baseImg->getStdImage(currWB, tr, origCropPart, ppP, true, params.toneCurve);


                            // we only need image reduced to 1/4 here
                            for (int ii = 0; ii < crH; ii += 2) {
                                for (int jj = 0; jj < crW; jj += 2) {
                                    provicalc->r(ii >> 1, jj >> 1) = origCropPart->r(ii, jj);
                                    provicalc->g(ii >> 1, jj >> 1) = origCropPart->g(ii, jj);
                                    provicalc->b(ii >> 1, jj >> 1) = origCropPart->b(ii, jj);
                                }
                            }

                            imgsrc->convertColorSpace(provicalc, params.icm, currWB);  //for denoise luminance curve
                            int nb = 0;
                            float chaut = 0.f, redaut = 0.f, blueaut = 0.f, maxredaut = 0.f, maxblueaut = 0.f, minredaut = 0.f, minblueaut = 0.f, chromina = 0.f, sigma = 0.f, lumema = 0.f, sigma_L = 0.f, redyel = 0.f, skinc = 0.f, nsknc = 0.f;
                            ipf.RGB_denoise_info(origCropPart, provicalc, imgsrc->isRAW(), gamcurve, gam, gamthresh, gamslope,  params.dirpyrDenoise, imgsrc->getDirPyrDenoiseExpComp(), chaut, nb, redaut, blueaut, maxredaut, maxblueaut, minredaut, minblueaut, chromina, sigma, lumema, sigma_L, redyel, skinc, nsknc);
                            Nb[hcr * 3 + wcr] = nb;
                            ch_M[hcr * 3 + wcr] = chaut;
                            max_r[hcr * 3 + wcr] = maxredaut;
                            max_b[hcr * 3 + wcr] = maxblueaut;
                            min_r[hcr * 3 + wcr] = minredaut;
                            min_b[hcr * 3 + wcr] = minblueaut;
                            lumL[hcr * 3 + wcr] = lumema;
                            chromC[hcr * 3 + wcr] = chromina;
                            ry[hcr * 3 + wcr] = redyel;
                            sk[hcr * 3 + wcr] = skinc;
                            pcsk[hcr * 3 + wcr] = nsknc;
                        }
                    }

                    delete provicalc;
                    delete origCropPart;
                }
                float chM = 0.f;
                float MaxR = 0.f;
                float MaxB = 0.f;
                float MinR = 100000000.f;
                float MinB = 100000000.f;
                float maxr = 0.f;
                float maxb = 0.f;
                float multip = 1.f;
                float adjustr = 1.f;
                float Max_R[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
                float Max_B[9] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
                float Min_R[9];
                float Min_B[9];
                float MaxRMoy = 0.f;
                float MaxBMoy = 0.f;
                float MinRMoy = 0.f;
                float MinBMoy = 0.f;

                if (params.icm.workingProfile == "ProPhoto")   {
                    adjustr = 1.f;
                } else if (params.icm.workingProfile == "Adobe RGB")  {
                    adjustr = 1.f / 1.3f;
                } else if (params.icm.workingProfile == "sRGB")       {
                    adjustr = 1.f / 1.3f;
                } else if (params.icm.workingProfile == "WideGamut")  {
                    adjustr = 1.f / 1.1f;
                } else if (params.icm.workingProfile == "Rec2020")  {
                    adjustr = 1.f / 1.1f;
                } else if (params.icm.workingProfile == "Beta RGB")   {
                    adjustr = 1.f / 1.2f;
                } else if (params.icm.workingProfile == "BestRGB")    {
                    adjustr = 1.f / 1.2f;
                } else if (params.icm.workingProfile == "BruceRGB")   {
                    adjustr = 1.f / 1.2f;
                }

                if (!imgsrc->isRAW()) {
                    multip = 2.f;    //take into account gamma for TIF / JPG approximate value...not good for gamma=1
                }

                float delta[9];
                int mode = 1;
                int lissage = settings->leveldnliss;

                for (int k = 0; k < 9; k++) {
                    float maxmax = max(max_r[k], max_b[k]);
                    ipf.calcautodn_info(ch_M[k], delta[k], Nb[k], levaut, maxmax, lumL[k], chromC[k], mode, lissage, ry[k], sk[k], pcsk[k]);
                    //  printf("ch_M=%f delta=%f\n",ch_M[k], delta[k]);
                }

                for (int k = 0; k < 9; k++) {
                    if (max_r[k] > max_b[k]) {
                        //printf("R delta=%f  koef=%f\n",delta[k],autoNRmax*multip*adjustr*lowdenoise);
                        Max_R[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                        Min_B[k] = - (ch_M[k] - min_b[k]) / (autoNRmax * multip * adjustr * lowdenoise);
                        Max_B[k] = 0.f;
                        Min_R[k] = 0.f;
                    } else {
                        //printf("B delta=%f  koef=%f\n",delta[k],autoNRmax*multip*adjustr*lowdenoise);
                        Max_B[k] = (delta[k]) / ((autoNRmax * multip * adjustr * lowdenoise) / 2.f);
                        Min_R[k] = - (ch_M[k] - min_r[k])   / (autoNRmax * multip * adjustr * lowdenoise);
                        Min_B[k] = 0.f;
                        Max_R[k] = 0.f;
                    }
                }

                for (int k = 0; k < 9; k++) {
                    //  printf("ch_M= %f Max_R=%f Max_B=%f min_r=%f min_b=%f\n",ch_M[k],Max_R[k], Max_B[k],Min_R[k], Min_B[k]);
                    chM += ch_M[k];
                    MaxBMoy += Max_B[k];
                    MaxRMoy += Max_R[k];
                    MinRMoy += Min_R[k];
                    MinBMoy += Min_B[k];

                    if (Max_R[k] > MaxR) {
                        MaxR = Max_R[k];
                    }

                    if (Max_B[k] > MaxB) {
                        MaxB = Max_B[k];
                    }

                    if (Min_R[k] < MinR) {
                        MinR = Min_R[k];
                    }

                    if (Min_B[k] < MinB) {
                        MinB = Min_B[k];
                    }

                }

                chM /= 9;
                MaxBMoy /= 9;
                MaxRMoy /= 9;
                MinBMoy /= 9;
                MinRMoy /= 9;

                if (MaxR > MaxB) {
                    maxr = MaxRMoy + (MaxR - MaxRMoy) * 0.66f; //#std Dev
                    //  maxb=MinB;
                    maxb = MinBMoy + (MinB - MinBMoy) * 0.66f;

                } else {
                    maxb = MaxBMoy + (MaxB - MaxBMoy) * 0.66f;
                    //  maxr=MinR;
                    maxr = MinRMoy + (MinR - MinRMoy) * 0.66f;

                }

//              printf("SIMPL cha=%f red=%f bl=%f \n",chM,maxr,maxb);

                params.dirpyrDenoise.chroma = chM / (autoNR * multip * adjustr);
                params.dirpyrDenoise.redchro = maxr;
                params.dirpyrDenoise.bluechro = maxb;
            }

            if (settings->verbose) {
                t2aue.set();
                printf("Info denoise auto performed in %d usec:\n", t2aue.etime(t1aue));
            }

            //end evaluate noise
        }

        baseImg = new Imagefloat(fw, fh);
        imgsrc->getImage(currWB, tr, baseImg, pp, params.toneCurve, params.raw);

        if (pl) {
            pl->setProgress(0.50);
        }

//  LUTf Noisecurve (65536,0);
//!!!// auto exposure!!!
        expcomp = params.toneCurve.expcomp;
        bright = params.toneCurve.brightness;
        contr = params.toneCurve.contrast;
        black = params.toneCurve.black;
        hlcompr = params.toneCurve.hlcompr;
        hlcomprthresh = params.toneCurve.hlcomprthresh;


        if (params.toneCurve.autoexp) {
            LUTu aehist;
            int aehistcompr;
            imgsrc->getAutoExpHistogram(aehist, aehistcompr);
            ipf.getAutoExp(aehist, aehistcompr, params.toneCurve.clip, expcomp, bright, contr, black, hlcompr, hlcomprthresh);
        }

        if (params.toneCurve.histmatching) {
            if (!params.toneCurve.fromHistMatching) {
                imgsrc->getAutoMatchedToneCurve(params.icm, params.raw, params.wb.observer, params.toneCurve.curve);
            }

            if (params.toneCurve.autoexp) {
                params.toneCurve.expcomp = 0.0;
            }

            params.toneCurve.autoexp = false;
            params.toneCurve.curveMode = ToneCurveMode::FILMLIKE;
            params.toneCurve.curve2 = { 0 };
            params.toneCurve.brightness = 0;
            params.toneCurve.contrast = 0;
            params.toneCurve.black = 0;
        }

        // Spot Removal
        if (params.spot.enabled && !params.spot.entries.empty()) {
            ipf.removeSpots(baseImg, imgsrc, params.spot.entries, pp, currWB, nullptr, tr);
        }

        // at this stage, we can flush the raw data to free up quite an important amount of memory
        // commented out because it makes the application crash when batch processing...
        // TODO: find a better place to flush rawData and rawRGB
        if (flush) {
            imgsrc->flush();
        }
    }

    /*
     * raw.base: the base image, denoise and source colour conversion. This is
     * one canonical block; the three calls are its real sub-blocks and run in
     * the order the hand-written pipeline always used.
     */
    void stage_raw_base()
    {
        stage::ScopedProfile node_profile("raw.base");

        stage_raw_base_source();
        stage_denoise();
        stage_raw_color();
    }

    void stage_denoise()
    {
        const procparams::ProcParams& params = job->pparams;

        DirPyrDenoiseParams denoiseParams = params.dirpyrDenoise;   // make a copy because we cheat here

        if (denoiseParams.Lmethod == "CUR") {
            if (noiseLCurve) {
                denoiseParams.luma = 0.5f;
            } else {
                denoiseParams.luma = 0.0f;
            }
        } else if (denoiseParams.Lmethod == "SLI") {
            noiseLCurve.Reset();
        }

        if (denoiseParams.enabled  && (noiseLCurve || noiseCCurve)) {
            // we only need image reduced to 1/4 here
            calclum = new Imagefloat((fw + 1) / 2, (fh + 1) / 2);  //for luminance denoise curve
#ifdef _OPENMP
            #pragma omp parallel for
#endif

            for (int ii = 0; ii < fh; ii += 2) {
                for (int jj = 0; jj < fw; jj += 2) {
                    calclum->r(ii >> 1, jj >> 1) = baseImg->r(ii, jj);
                    calclum->g(ii >> 1, jj >> 1) = baseImg->g(ii, jj);
                    calclum->b(ii >> 1, jj >> 1) = baseImg->b(ii, jj);
                }
            }

            imgsrc->convertColorSpace(calclum, params.icm, currWB);
        }

        if (denoiseParams.enabled) {
            ImProcFunctions &ipf = * (ipf_p.get());
            float nresi, highresi;
            int kall = 2;
            ipf.RGB_denoise(kall, baseImg, baseImg, calclum, ch_M, max_r, max_b, imgsrc->isRAW(), denoiseParams, imgsrc->getDirPyrDenoiseExpComp(), noiseLCurve, noiseCCurve, nresi, highresi);

        }

//  delete calclum;
        delete [] ch_M;
        delete [] max_r;
        delete [] max_b;
        delete [] min_r;
        delete [] min_b;
        delete [] lumL;
        delete [] chromC;
        delete [] ry;
        delete [] sk;
        delete [] pcsk;
    }

    /*
     * raw.base, source colour: convert to the working space, optionally
     * through film-negative processing, then compress the source gamut.
     * Extracted from stage_transform() unchanged. The histogram allocation
     * moved to stage_analysis() so this block ends before analysis begins; it
     * only allocates a LUT, so the pixel order is unchanged.
     */
    void stage_raw_color()
    {
        const procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        if (params.filmNegative.enabled) {
            // Process film negative AFTER colorspace conversion if camera space is NOT selected
            if (params.filmNegative.colorSpace != FilmNegativeParams::ColorSpace::INPUT) {
                imgsrc->convertColorSpace(baseImg, params.icm, currWB);
            }

            FilmNegativeParams copy = params.filmNegative;
            ipf.filmNegativeProcess(baseImg, baseImg, copy, params.raw, imgsrc, currWB);

            // ... otherwise, process film negative BEFORE colorspace conversion
            if (params.filmNegative.colorSpace == FilmNegativeParams::ColorSpace::INPUT) {
                imgsrc->convertColorSpace(baseImg, params.icm, currWB);
            }

        } else {
            imgsrc->convertColorSpace(baseImg, params.icm, currWB);
        }

        if (params.cg.enabled) {//gamut compression
            float mac = 0.f;
            float mac0 = 0.f;
            float mac1 = 0.f;
            float mac2 = 0.f;
            int beginend = 0;
            int nbsegam = 0;
            float powe = 1.f;
            ipf.gamutcompr(baseImg, baseImg, beginend, powe, nbsegam, mac, mac0, mac1, mac2);
        }
    }

    /*
     * analysis: allocate the working histogram and run the first analysis.
     * Extracted from stage_transform() unchanged.
     */
    void stage_analysis()
    {
        stage::ScopedProfile node_profile("analysis");

        const procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        hist16(65536);
        ipf.firstAnalysis(baseImg, params, hist16);
    }

    /*
     * transform: dehaze and Fattal tone mapping, then the geometry transform.
     * The two tone operations run after firstAnalysis() in the implementation
     * and so are the prologue of this node rather than part of raw.base; see
     * devnotes/perf/STAGE_RUNTIME.md.
     */
    void stage_transform()
    {
        stage::ScopedProfile node_profile("transform");

        const procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        ipf.dehaze(baseImg, params.dehaze);
        ipf.ToneMapFattal02(baseImg, params.fattal, 3, 0, nullptr, 0, 0, 0, false);

        // perform transform (excepted resizing)
        if (ipf.needsTransform(fw, fh, imgsrc->getRotateDegree(), imgsrc->getMetaData())) {
            Imagefloat* trImg = nullptr;

            if (ipf.needsLuminanceOnly()) {
                trImg = baseImg;
            } else {
                trImg = new Imagefloat(fw, fh);
            }

            ipf.transform(baseImg, trImg, 0, 0, 0, 0, fw, fh, fw, fh,
                          imgsrc->getMetaData(), imgsrc->getRotateDegree(), true, true);

            if (trImg != baseImg) {
                delete baseImg;
                baseImg = trImg;
            }
        }
    }

    /*
     * CIE CAM 02 is active when it is enabled and set to the 02 model. Several
     * blocks test it, so the predicate has one definition instead of a local
     * copy recomputed in each node.
     */
    bool usesCieCam02() const
    {
        const procparams::ProcParams& params = job->pparams;
        return params.colorappearance.modelmethod == "02" && params.colorappearance.enabled;
    }

    /*
     * Finish sequence of the fast export pipeline. The graph calls the same
     * node methods through the inventory; this wrapper keeps the fast path's
     * fixed order in one place.
     */
    Imagefloat *stage_finish()
    {
        stage_local_adjustments();
        stage_rgb_proc();
        stage_lab_tools();
        stage_lab_sharpening();
        stage_lab_wavelet();
        stage_working_trc();
        stage_ciecam();
        stage_gamut();
        stage_crop();
        stage_resize();
        stage_output_sharpen();
        stage_output_convert();
        stage_framing();

        return stage_output_icc();
    }

    /*
     * local.adjustments: the local-lab parameter fixups, the optional
     * before-CBDL directional pyramid equaliser and Lab_Local() over every
     * spot. Extracted from stage_finish() unchanged.
     */
    void stage_local_adjustments()
    {
        stage::ScopedProfile node_profile("local.adjustments");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        /*
         * The node is opt-in and its colour-only device path is not
         * implemented yet: the context and its predicate are in place, but
         * the launcher refuses before it touches the Lab buffer, so the
         * builtin runs the extracted host block and records impl=host. The
         * block is atomic: `supported` is the whole-block predicate, and no
         * spot is ever split between the two implementations.
         */
        if (stage::node_device("local.adjustments") == stage::StageNodeDevice::OpenCL) {
            if (labView == nullptr) {
                labView = new LabImage(fw, fh);
            }

            ipf.rgb2lab(*baseImg, *labView, params.icm.workingProfile);

            stage::LocalAdjustmentsContext context(params, ipf, baseImg,
                                                   labView, fw, fh);
            const bool supported = stage::localAdjustmentsSupported(params);
            RtLocalAdjustmentsContext descriptor =
                context.descriptor(supported);

            RtStageArg args[3] = {};
            args[0].kind = RT_STAGE_ARG_BUFFER;
            args[0].u.buffer = stage::view(*labView);
            args[1].kind = RT_STAGE_ARG_PTR;
            args[1].u.ptr = &descriptor;
            args[2].kind = RT_STAGE_ARG_INT;
            args[2].u.i = supported ? 1 : 0;

            RtStageProfiling profiling{};
            if (stage::run("local.adjustments",
                           std::vector<RtStageArg>(args, args + 3),
                           &profiling) == RT_STAGE_OK) {
                /* The node returns Lab; baseImg stays the host's. */
                ipf.lab2rgb(*labView, *baseImg, params.icm.workingProfile);
                return;
            }
        }

        if (labView == nullptr) {
            labView = new LabImage(fw, fh);
        }
        stage::localAdjustmentsHostBlock(ipf, params, baseImg, labView, fw,
                                         fh);
    }

    /*
     * rgb.proc: tone and RGB curve setup, colour-toning curves, black and
     * white conversion, saturation limits and rgbProc(). Extracted from
     * stage_finish() unchanged; the base image is released afterwards.
     */
    void stage_rgb_proc()
    {
        stage::ScopedProfile node_profile("rgb.proc");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        curve1(65536);
        curve2(65536);
        curve(65536, 0);
        satcurve(65536, 0);
        lhskcurve(65536, 0);
        lumacurve(32770, 0);  // lumacurve[32768] and lumacurve[32769] will be set to 32768 and 32769 later to allow linear interpolation
        clcurve(65536, 0);
        wavclCurve(65536, 0);

        //if(params.blackwhite.enabled) params.toneCurve.hrenabled=false;

        CurveFactory::complexCurve(expcomp, black / 65535.0, hlcompr, hlcomprthresh, params.toneCurve.shcompr, bright, contr,
                                   params.toneCurve.curve, params.toneCurve.curve2,
                                   hist16, curve1, curve2, curve, dummy, customToneCurve1, customToneCurve2);

        CurveFactory::RGBCurve(params.rgbCurves.rcurve, rCurve, 1);
        CurveFactory::RGBCurve(params.rgbCurves.gcurve, gCurve, 1);
        CurveFactory::RGBCurve(params.rgbCurves.bcurve, bCurve, 1);

        bool opautili = false;

        if (params.colorToning.enabled) {
            TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params.icm.workingProfile);
            double wp[3][3] = {
                {wprof[0][0], wprof[0][1], wprof[0][2]},
                {wprof[1][0], wprof[1][1], wprof[1][2]},
                {wprof[2][0], wprof[2][1], wprof[2][2]}
            };
            params.colorToning.getCurves(ctColorCurve, ctOpacityCurve, wp, opautili);
            clToningcurve(65536, 0);
            CurveFactory::diagonalCurve2Lut(params.colorToning.clcurve, clToningcurve, 1);
            cl2Toningcurve(65536, 0);
            CurveFactory::diagonalCurve2Lut(params.colorToning.cl2curve, cl2Toningcurve, 1);
        }

//        labView = new LabImage(fw, fh);

        if (params.blackwhite.enabled) {
            CurveFactory::curveBW(params.blackwhite.beforeCurve, params.blackwhite.afterCurve, hist16, dummy, customToneCurvebw1, customToneCurvebw2, 1);
        }

        double rrm, ggm, bbm;
        float autor, autog, autob;
        float satLimit = float (params.colorToning.satProtectionThreshold) / 100.f * 0.7f + 0.3f;
        float satLimitOpacity = 1.f - (float (params.colorToning.saturatedOpacity) / 100.f);

        if (params.colorToning.enabled  && params.colorToning.autosat && params.colorToning.method != "LabGrid") { //for colortoning evaluation of saturation settings
            float moyS = 0.f;
            float eqty = 0.f;
            ipf.moyeqt(baseImg, moyS, eqty); //return image : mean saturation and standard dev of saturation
            float satp = ((moyS + 1.5f * eqty) - 0.3f) / 0.7f; //1.5 sigma ==> 93% pixels with high saturation -0.3 / 0.7 convert to Hombre scale

            if (satp >= 0.92f) {
                satp = 0.92f;    //avoid values too high (out of gamut)
            }

            if (satp <= 0.15f) {
                satp = 0.15f;    //avoid too low values
            }

            satLimit = 100.f * satp;

            satLimitOpacity = 100.f * (moyS - 0.85f * eqty); //-0.85 sigma==>20% pixels with low saturation
        }

        autor = -9000.f; // This will ask to compute the "auto" values for the B&W tool (have to be inferior to -5000)
        DCPProfileApplyState as;
        DCPProfile *dcpProf = imgsrc->getDCP(params.icm, as);

        LUTu histToneCurve;

        const auto& options = App::get().options();
        {
            stage::ScopedProfile profile("finish.rgbproc");
            ipf.rgbProc(baseImg, labView, nullptr, curve1, curve2, curve, params.toneCurve.saturation, rCurve, gCurve, bCurve, satLimit, satLimitOpacity, ctColorCurve, ctOpacityCurve, opautili, clToningcurve, cl2Toningcurve, customToneCurve1, customToneCurve2, customToneCurvebw1, customToneCurvebw2, rrm, ggm, bbm, autor, autog, autob, expcomp, hlcompr, hlcomprthresh, dcpProf, as, histToneCurve, options.chunkSizeRGB, options.measure);
        }

        if (settings->verbose) {
            printf("Output image / Auto B&W coefs:   R=%.2f   G=%.2f   B=%.2f\n", static_cast<double>(autor), static_cast<double>(autog), static_cast<double>(autob));
        }

        // if clut was used and size of clut cache == 1 we free the memory used by the clutstore (default clut cache size = 1 for 32 bit OS)
        if (params.filmSimulation.enabled && !params.filmSimulation.clutFilename.empty() && options.clutCacheSize == 1) {
            CLUTStore::getInstance().clearCache();
        }

        // freeing up some memory
        customToneCurve1.Reset();
        customToneCurve2.Reset();
        ctColorCurve.Reset();
        ctOpacityCurve.Reset();
        noiseLCurve.Reset();
        noiseCCurve.Reset();
        customToneCurvebw1.Reset();
        customToneCurvebw2.Reset();

        // Freeing baseImg because not used anymore
        delete baseImg;
        baseImg = nullptr;

        if (pl) {
            pl->setProgress(0.55);
        }
    }

    /*
     * lab.tools: the Lab curve tables, Lab-grid colour toning, shadows and
     * highlights, local contrast, the chrominance/luminance curve, EPD tone
     * mapping, vibrance and Lab colour-correction regions. Extracted from
     * stage_finish() unchanged.
     */
    void stage_lab_tools()
    {
        stage::ScopedProfile node_profile("lab.tools");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        stage::LabToolsLuts luts{&hist16, &lumacurve, &dummy, &clcurve,
                                 &curve1, &curve2, &satcurve, &lhskcurve};

        /*
         * The device node is opt-in and covers the LabGrid pass,
         * shadows/highlights, the Lab curves, vibrance, local contrast and the
         * bounded Lab-region set. Everything else in the block is a residual:
         * the node is not submitted as supported, the launcher refuses it and
         * the builtin below runs the host block, recording impl=host. The
         * launcher also refuses vibrance, the Lab-curve pass, local contrast
         * and regions on a GPU device, whose arithmetic rounding can move a
         * Munsell-limit pixel or the region mask, and the Lab-curve,
         * local-contrast and region passes on a device without fp64.
         */
        if (stage::node_device("lab.tools") == stage::StageNodeDevice::OpenCL) {
            const bool supported = stage::labToolsSupported(params);
            stage::LabToolsContext context(params, ipf, luts, autili, butili);
            RtLabToolsResult setup{};
            const bool deviceReady = supported && context.build(setup) == 0;
            RtLabToolsContext descriptor = context.descriptor(deviceReady);

            RtStageArg args[69] = {};
            args[0].kind = RT_STAGE_ARG_BUFFER;
            args[0].u.buffer = stage::view(*labView);
            args[1].kind = RT_STAGE_ARG_PTR;
            args[1].u.ptr = &descriptor;
            args[2].kind = RT_STAGE_ARG_INT;
            args[2].u.i = deviceReady ? 1 : 0;
            args[3].kind = RT_STAGE_ARG_INT;
            args[3].u.i = (params.colorToning.enabled &&
                           params.colorToning.method == "LabGrid")
                              ? 1
                              : 0;
            for (int index = 0; index < 4; ++index) {
                args[4 + index].kind = RT_STAGE_ARG_DOUBLE;
                args[4 + index].u.f =
                    deviceReady ? setup.labgrid[index] : 0.0;
            }
            args[8].kind = RT_STAGE_ARG_INT;
            args[8].u.i = params.sh.enabled ? 1 : 0;
            args[9].kind = RT_STAGE_ARG_INT;
            args[9].u.i = params.sh.highlights;
            args[10].kind = RT_STAGE_ARG_INT;
            args[10].u.i = params.sh.shadows;
            args[11].kind = RT_STAGE_ARG_INT;
            args[11].u.i = params.sh.htonalwidth;
            args[12].kind = RT_STAGE_ARG_INT;
            args[12].u.i = params.sh.stonalwidth;
            args[13].kind = RT_STAGE_ARG_INT;
            args[13].u.i = params.sh.radius;
            args[14].kind = RT_STAGE_ARG_INT;
            args[14].u.i = params.sh.lab ? 1 : 0;
            args[15].kind = RT_STAGE_ARG_INT;
            args[15].u.i = stage::LAB_TOOLS_EXPORT_SCALE;

            /*
             * Vibrance's own parameters. The flag is the same predicate the
             * supported set uses, so a vibrance block that would not touch a
             * pixel never reaches the launcher as a device tool.
             */
            const bool vibrance = stage::labToolsVibranceActive(params);
            args[22].kind = RT_STAGE_ARG_INT;
            args[22].u.i = deviceReady && vibrance ? 1 : 0;
            args[23].kind = RT_STAGE_ARG_INT;
            args[23].u.i = params.vibrance.pastels;
            args[24].kind = RT_STAGE_ARG_INT;
            args[24].u.i = params.vibrance.saturated;
            args[25].kind = RT_STAGE_ARG_INT;
            args[25].u.i = params.vibrance.psthreshold.getTopLeft();
            args[26].kind = RT_STAGE_ARG_INT;
            args[26].u.i = params.vibrance.psthreshold.getBottomLeft();
            args[27].kind = RT_STAGE_ARG_INT;
            args[27].u.i = params.vibrance.protectskins ? 1 : 0;
            args[28].kind = RT_STAGE_ARG_INT;
            args[28].u.i = params.vibrance.avoidcolorshift ? 1 : 0;
            args[29].kind = RT_STAGE_ARG_INT;
            args[29].u.i = deviceReady && vibrance &&
                                  setup.skin_curve_set != 0
                              ? 1
                              : 0;
            args[30].kind = RT_STAGE_ARG_INT;
            args[30].u.i = params.toneCurve.hrenabled ? 1 : 0;

            /*
             * The Lab-curve pass. The six activity flags and the tables come
             * from the host build; the method, the red/skin settings and the
             * chroma scale are read from the same values the block reads.
             */
            args[34].kind = RT_STAGE_ARG_INT;
            args[34].u.i = deviceReady ? setup.lab_curve_active : 0;
            args[35].kind = RT_STAGE_ARG_INT;
            args[35].u.i = deviceReady ? setup.lab_curve_l_active : 0;
            args[36].kind = RT_STAGE_ARG_INT;
            args[36].u.i = deviceReady ? setup.lab_curve_a_active : 0;
            args[37].kind = RT_STAGE_ARG_INT;
            args[37].u.i = deviceReady ? setup.lab_curve_b_active : 0;
            args[38].kind = RT_STAGE_ARG_INT;
            args[38].u.i = deviceReady ? setup.lab_curve_cc_active : 0;
            args[39].kind = RT_STAGE_ARG_INT;
            args[39].u.i = deviceReady ? setup.lab_curve_lc_active : 0;
            args[40].kind = RT_STAGE_ARG_INT;
            args[40].u.i = deviceReady ? setup.lab_curve_cl_active : 0;
            args[41].kind = RT_STAGE_ARG_INT;
            args[41].u.i = deviceReady ? setup.lab_curve_munsell : 0;
            args[42].kind = RT_STAGE_ARG_INT;
            args[42].u.i = deviceReady ? setup.lab_curve_lcredsk : 0;
            const double lab_curve_doubles[5] = {
                deviceReady ? setup.lab_curve_rst_protection : 0.0,
                deviceReady ? setup.lab_curve_protect_red : 0.0,
                deviceReady ? setup.lab_curve_protect_red_h : 0.0,
                deviceReady ? setup.lab_curve_amount_chroma : 0.0,
                deviceReady ? setup.lab_curve_adjust : 1.0};
            for (int index = 0; index < 5; ++index) {
                args[43 + index].kind = RT_STAGE_ARG_DOUBLE;
                args[43 + index].u.f = lab_curve_doubles[index];
            }
            for (int index = 48; index < 54; ++index) {
                args[index].kind = RT_STAGE_ARG_BUFFER;
            }
            args[54].kind = RT_STAGE_ARG_INT;
            args[54].u.i = deviceReady
                               ? static_cast<std::int64_t>(
                                     setup.lab_curve_l_entries)
                               : 0;
            args[55].kind = RT_STAGE_ARG_INT;
            args[55].u.i = deviceReady
                               ? static_cast<std::int64_t>(
                                     setup.lab_curve_entries)
                               : 0;

            /*
             * Local contrast. The flag is the context's active flag; the three
             * sliders arrive as the block's own float narrowing and the
             * coefficients as a host-only pointer.
             */
            args[56].kind = RT_STAGE_ARG_INT;
            args[56].u.i = deviceReady ? setup.local_contrast_enabled : 0;
            args[57].kind = RT_STAGE_ARG_INT;
            args[57].u.i = deviceReady
                               ? static_cast<std::int64_t>(
                                     setup.local_contrast_radius)
                               : 0;
            args[58].kind = RT_STAGE_ARG_DOUBLE;
            args[58].u.f = deviceReady ? setup.local_contrast_amount : 0.0;
            args[59].kind = RT_STAGE_ARG_DOUBLE;
            args[59].u.f =
                deviceReady ? setup.local_contrast_darkness : 1.0;
            args[60].kind = RT_STAGE_ARG_DOUBLE;
            args[60].u.f =
                deviceReady ? setup.local_contrast_lightness : 1.0;
            args[61].kind = RT_STAGE_ARG_INT;
            args[61].u.i = deviceReady
                               ? static_cast<std::int64_t>(
                                     setup.local_contrast_scale)
                               : stage::LAB_TOOLS_EXPORT_SCALE;
            args[62].kind = RT_STAGE_ARG_PTR;
            args[62].u.ptr =
                deviceReady ? setup.local_contrast_iir : nullptr;

            /*
             * Lab regions. The count is the context's region set; the mask
             * tables and per-region floats are uploaded from the host build.
             */
            args[63].kind = RT_STAGE_ARG_INT;
            args[63].u.i = deviceReady ? setup.region_count : 0;
            args[64].kind = RT_STAGE_ARG_BUFFER;
            args[65].kind = RT_STAGE_ARG_INT;
            args[65].u.i = deviceReady
                               ? static_cast<std::int64_t>(
                                     setup.region_lut_stride)
                               : 0;
            args[66].kind = RT_STAGE_ARG_BUFFER;
            args[67].kind = RT_STAGE_ARG_PTR;
            args[67].u.ptr = deviceReady ? setup.ws_double : nullptr;
            args[68].kind = RT_STAGE_ARG_PTR;
            args[68].u.ptr = deviceReady ? setup.iws_double : nullptr;

            for (int index = 16; index < 22; ++index) {
                args[index].kind = RT_STAGE_ARG_BUFFER;
            }
            for (int index = 31; index < 34; ++index) {
                args[index].kind = RT_STAGE_ARG_BUFFER;
            }
            if (deviceReady) {
                const auto host_buffer = [](const float* data, int width) {
                    RtStageBuffer buffer{};
                    buffer.size = sizeof(RtStageBuffer);
                    buffer.data = const_cast<float*>(data);
                    buffer.width = width;
                    buffer.height = 1;
                    buffer.row_pitch = static_cast<std::size_t>(width) *
                                       sizeof(float);
                    buffer.plane_pitch = buffer.row_pitch;
                    buffer.planes = 1;
                    buffer.format = RT_STAGE_F32;
                    buffer.flags = RT_STAGE_BUFFER_READ;
                    return buffer;
                };
                args[16].u.buffer = host_buffer(
                    setup.lut_highlight, static_cast<int>(setup.lut_entries));
                args[17].u.buffer = host_buffer(
                    setup.lut_shadow, static_cast<int>(setup.lut_entries));
                args[18].u.buffer = host_buffer(setup.ws, 9);
                args[19].u.buffer = host_buffer(setup.iws, 9);
                args[20].u.buffer = host_buffer(
                    setup.cachef, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));
                args[21].u.buffer = host_buffer(
                    setup.cachefy, static_cast<int>(RT_LAB_TOOLS_CACHE_ENTRIES));
                if (vibrance) {
                    args[31].u.buffer = host_buffer(
                        setup.munsell,
                        static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES *
                                         RT_LAB_TOOLS_MUNSELL_STRIDE));
                    args[32].u.buffer = host_buffer(
                        setup.munsell_sizes,
                        static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES));
                    args[33].u.buffer = host_buffer(
                        setup.skin_curve,
                        setup.skin_curve_set != 0
                            ? static_cast<int>(
                                  RT_LAB_TOOLS_SKIN_CURVE_ENTRIES)
                            : 1);
                } else if (setup.lab_curve_munsell != 0) {
                    args[31].u.buffer = host_buffer(
                        setup.munsell,
                        static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES *
                                         RT_LAB_TOOLS_MUNSELL_STRIDE));
                    args[32].u.buffer = host_buffer(
                        setup.munsell_sizes,
                        static_cast<int>(RT_LAB_TOOLS_MUNSELL_TABLES));
                }
                if (setup.region_count != 0) {
                    args[64].u.buffer = host_buffer(
                        setup.region_luts,
                        static_cast<int>(
                            setup.region_count *
                            RT_LAB_TOOLS_REGION_CURVES *
                            setup.region_lut_stride));
                    args[66].u.buffer = host_buffer(
                        setup.region_params,
                        static_cast<int>(
                            setup.region_count *
                            RT_LAB_TOOLS_REGION_PARAM_STRIDE));
                }
                if (setup.lab_curve_active != 0) {
                    args[48].u.buffer = host_buffer(
                        setup.lab_curve_l,
                        static_cast<int>(setup.lab_curve_l_entries));
                    args[49].u.buffer = host_buffer(
                        setup.lab_curve_a,
                        static_cast<int>(setup.lab_curve_entries));
                    args[50].u.buffer = host_buffer(
                        setup.lab_curve_b,
                        static_cast<int>(setup.lab_curve_entries));
                    args[51].u.buffer = host_buffer(
                        setup.lab_curve_cc,
                        static_cast<int>(setup.lab_curve_entries));
                    args[52].u.buffer = host_buffer(
                        setup.lab_curve_lc,
                        static_cast<int>(setup.lab_curve_entries));
                    args[53].u.buffer = host_buffer(
                        setup.lab_curve_cl,
                        static_cast<int>(setup.lab_curve_entries));
                }
            }

            RtStageProfiling profiling{};
            if (stage::run("lab.tools",
                           std::vector<RtStageArg>(args, args + 69),
                           &profiling) == RT_STAGE_OK) {
                return;
            }
        }

        stage::labToolsHostBlock(ipf, params, *labView, luts, autili, butili);
    }

    /*
     * lab.sharpening: impulse denoise, defringe, edge and microcontrast
     * sharpening and the main sharpening pass. Extracted from stage_finish()
     * unchanged.
     */
    void stage_lab_sharpening()
    {
        stage::ScopedProfile node_profile("lab.sharpening");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());
        const bool cam02 = usesCieCam02();

        // for all treatments Defringe, Sharpening, Contrast detail ,Microcontrast they are activated if "CIECAM" function are disabled

      //  if ((params.colorappearance.enabled && !settings->autocielab) || (!params.colorappearance.enabled)) {
        if ((params.colorappearance.enabled && !settings->autocielab) || (!cam02)) {
            ipf.impulsedenoise(labView);
            ipf.defringe(labView);
        }

        if (params.sharpenEdge.enabled) {
            ipf.MLsharpen(labView);
        }

        if (params.sharpenMicro.enabled) {
          //  if ((params.colorappearance.enabled && !settings->autocielab) || (!params.colorappearance.enabled)) {
            if ((params.colorappearance.enabled && !settings->autocielab) || (!cam02)) {
                ipf.MLmicrocontrast(labView);     //!params.colorappearance.sharpcie
            }
        }

      //  if (((params.colorappearance.enabled && !settings->autocielab) || (!params.colorappearance.enabled)) && params.sharpening.enabled) {
        if (((params.colorappearance.enabled && !settings->autocielab) || (!cam02)) && params.sharpening.enabled) {
            stage::ScopedProfile profile("finish.sharpening");
            ipf.sharpening(labView, params.sharpening);

        }
    }

    /*
     * lab.wavelet: the after-CBDL directional pyramid equaliser and the
     * wavelet tool's contrast-only path. Either pass may run in the node; the
     * extracted host block runs both when the node is a residual.
     */
    void stage_lab_wavelet()
    {
        stage::ScopedProfile node_profile("lab.wavelet");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        /*
         * The device node is opt-in. It covers the directional pyramid
         * equaliser in after-CBDL position and the wavelet tool's contrast-only
         * path, either alone or in the block's order. Everything else is a
         * residual: the node is not submitted as supported, the launcher
         * refuses it and the builtin runs the host block, recording
         * impl=host. The launcher also refuses a GPU device, whose rounding
         * can move a hipass across a range-table entry, and a wavelet pass
         * without fp64.
         */
        if (stage::node_device("lab.wavelet") == stage::StageNodeDevice::OpenCL) {
            const bool supported =
                stage::labWaveletSupported(params, labView->W, labView->H);
            stage::LabWaveletContext context(params, ipf, labView->W, labView->H);
            RtLabWaveletResult setup{};
            const bool deviceReady = supported && context.build(setup) == 0;
            RtLabWaveletContext descriptor = context.descriptor(deviceReady);

            RtStageArg args[22] = {};
            args[0].kind = RT_STAGE_ARG_BUFFER;
            args[0].u.buffer = stage::view(*labView);
            args[1].kind = RT_STAGE_ARG_PTR;
            args[1].u.ptr = &descriptor;
            args[2].kind = RT_STAGE_ARG_INT;
            args[2].u.i = deviceReady ? 1 : 0;
            for (int level = 0; level < 6; ++level) {
                args[3 + level].kind = RT_STAGE_ARG_DOUBLE;
                args[3 + level].u.f = params.dirpyrequalizer.mult[level];
            }
            args[9].kind = RT_STAGE_ARG_DOUBLE;
            args[9].u.f = params.dirpyrequalizer.threshold;
            args[10].kind = RT_STAGE_ARG_DOUBLE;
            args[10].u.f = params.dirpyrequalizer.skinprotect;
            args[11].kind = RT_STAGE_ARG_DOUBLE;
            args[11].u.f = static_cast<double>(params.dirpyrequalizer.hueskin.getBottomLeft()) / 100.0;
            args[12].kind = RT_STAGE_ARG_DOUBLE;
            args[12].u.f = static_cast<double>(params.dirpyrequalizer.hueskin.getTopLeft()) / 100.0;
            args[13].kind = RT_STAGE_ARG_DOUBLE;
            args[13].u.f = static_cast<double>(params.dirpyrequalizer.hueskin.getTopRight()) / 100.0;
            args[14].kind = RT_STAGE_ARG_INT;
            args[14].u.i = params.dirpyrequalizer.gamutlab ? 1 : 0;
            args[15].kind = RT_STAGE_ARG_INT;
            args[15].u.i = stage::LAB_WAVELET_EXPORT_SCALE;
            args[16].kind = RT_STAGE_ARG_INT;
            args[16].u.i = deviceReady ? setup.levels : 0;
            for (int index = 17; index < 20; ++index) {
                args[index].kind = RT_STAGE_ARG_BUFFER;
            }
            args[20].kind = RT_STAGE_ARG_INT;
            args[20].u.i = deviceReady && setup.wavelet != 0 ? 1 : 0;
            args[21].kind = RT_STAGE_ARG_BUFFER;
            if (deviceReady) {
                const auto host_buffer = [](const float* data, int width) {
                    RtStageBuffer buffer{};
                    buffer.size = sizeof(RtStageBuffer);
                    buffer.data = const_cast<float*>(data);
                    buffer.width = width;
                    buffer.height = 1;
                    buffer.row_pitch = static_cast<std::size_t>(width) * sizeof(float);
                    buffer.plane_pitch = buffer.row_pitch;
                    buffer.planes = 1;
                    buffer.format = RT_STAGE_F32;
                    buffer.flags = RT_STAGE_BUFFER_READ;
                    return buffer;
                };
                if (setup.levels > 0) {
                    args[17].u.buffer = host_buffer(
                        setup.luts,
                        static_cast<int>(setup.levels * RT_LAB_WAVELET_LUT_ENTRIES));
                }
                if (setup.skinprotect != 0) {
                    const auto mask_buffer = [](const float* data, int width,
                                                int height) {
                        RtStageBuffer buffer{};
                        buffer.size = sizeof(RtStageBuffer);
                        buffer.data = const_cast<float*>(data);
                        buffer.width = width;
                        buffer.height = height;
                        buffer.row_pitch =
                            static_cast<std::size_t>(width) * sizeof(float);
                        buffer.plane_pitch =
                            buffer.row_pitch * static_cast<std::size_t>(height);
                        buffer.planes = 1;
                        buffer.format = RT_STAGE_F32;
                        buffer.flags = RT_STAGE_BUFFER_READ;
                        return buffer;
                    };
                    args[18].u.buffer = mask_buffer(
                        setup.hue, labView->W, labView->H);
                    args[19].u.buffer = mask_buffer(
                        setup.chrom, labView->W, labView->H);
                }
                if (setup.wavelet != 0 && setup.params != nullptr) {
                    args[21].u.buffer = host_buffer(
                        setup.params, static_cast<int>(setup.wavelet_entries));
                }
            }

            RtStageProfiling profiling{};
            if (stage::run("lab.wavelet",
                           std::vector<RtStageArg>(args, args + 22),
                           &profiling) == RT_STAGE_OK) {
                return;
            }
        }

        stage::labWaveletHostBlock(ipf, params, *labView);
    }

    /*
     * working.trc: soft light, the working-TRC conversion with its local
     * contrast, tone equaliser, saturation, preservation and black-and-white
     * output. Extracted from stage_finish() unchanged.
     */
    void stage_working_trc()
    {
        stage::ScopedProfile node_profile("working.trc");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        /*
         * The device node is opt-in. Its host prefix runs here, before the
         * submission: lab2rgb() and the fixed sRGB inverse are the block's own
         * code, so the probe, the profile and the second-pass input are
         * byte-identical to the CPU block whatever the device compiler does.
         * With RT_STAGE_OPENCL unset the block below is the pre-node path.
         */
        if (stage::node_device("working.trc") == stage::StageNodeDevice::OpenCL) {
            const bool supported = stage::workingTrcSupported(params);
            stage::WorkingTrcContext context(params, ipf);
            RtWorkingTrcContext descriptor = context.descriptor(supported);

            RtStageArg args[6] = {};
            args[0].kind = RT_STAGE_ARG_BUFFER;
            args[0].u.buffer = stage::view(*labView);
            args[1].kind = RT_STAGE_ARG_PTR;
            args[1].u.ptr = &descriptor;
            args[2].kind = RT_STAGE_ARG_INT;
            args[2].u.i = supported ? 1 : 0;
            args[3].kind = RT_STAGE_ARG_DOUBLE;
            args[3].u.f = params.icm.wapsat;

            bool deviceReady = false;
            if (supported) {
                RtWorkingTrcResult setup{};
                deviceReady = context.prepare(*labView) &&
                              context.build(setup) == 0;
                if (deviceReady) {
                    args[4].kind = RT_STAGE_ARG_BUFFER;
                    args[4].u.buffer = stage::view(context.xyz());
                    args[5].kind = RT_STAGE_ARG_BUFFER;
                    args[5].u.buffer = stage::view(context.linearReference());
                }
            }
            /*
             * A residual configuration, or a supported one whose setup the
             * host had to refuse, is still submitted with the support flag
             * clear: the launcher refuses it and the builtin runs the host
             * block, which records impl=host.
             */
            descriptor.supported = deviceReady ? 1u : 0u;
            args[2].u.i = descriptor.supported;

            RtStageProfiling profiling{};
            if (stage::run("working.trc",
                           std::vector<RtStageArg>(args, args + 6),
                           &profiling) == RT_STAGE_OK) {
                return;
            }
        }

        stage::workingTrcHostBlock(ipf, params, *labView);
    }

    /*
     * ciecam: the colour-appearance model and its tone mapping. Extracted
     * from stage_finish() unchanged.
     */
    void stage_ciecam()
    {
        stage::ScopedProfile node_profile("ciecam");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        //Colorappearance and tone-mapping associated

        int f_w = 1, f_h = 1;

        if (params.colorappearance.tonecie || params.colorappearance.enabled) {
            f_w = fw;
            f_h = fh;
        }

        CieImage *cieView = new CieImage(f_w, (f_h));

        CurveFactory::curveLightBrightColor(
            params.colorappearance.curve,
            params.colorappearance.curvered,
            params.colorappearance.curvegreen,
            params.colorappearance.curveblue,
            params.colorappearance.curve2,
            params.colorappearance.curve3,
            hist16, dummy,
            dummy, dummy,
            customColCurve1,
            customColCurvered,
            customColCurvegreen,
            customColCurveblue,
            customColCurve2,
            customColCurve3,
            1);

        if (params.colorappearance.enabled) {
            double adap;

            const float fnum = imgsrc->getMetaData()->getFNumber();         // F number
            const float fiso = imgsrc->getMetaData()->getISOSpeed() ;       // ISO
            const float fspeed = imgsrc->getMetaData()->getShutterSpeed() ; // Speed
            const float fcomp = imgsrc->getMetaData()->getExpComp();        // Compensation + -

            if (fnum < 0.3f || fiso < 5.f || fspeed < 0.00001f) {
                adap = 2000.;
            }//if no exif data or wrong
            else {
                double E_V = fcomp + log2((fnum * fnum) / fspeed / (fiso / 100.f));
                double kexp = 0.;
                E_V += kexp * params.toneCurve.expcomp;// exposure compensation in tonecurve ==> direct EV
                E_V += 0.5 * log2(params.raw.expos); // exposure raw white point ; log2 ==> linear to EV
                adap = std::pow(2.0, E_V - 3.0); //cd / m2
            }

            LUTf CAMBrightCurveJ;
            LUTf CAMBrightCurveQ;
            float CAMMean = NAN;

            float d, dj, yb;
            ipf.ciecam_02float(cieView, float (adap), 1, 2, labView, &params, customColCurve1, customColCurvered, customColCurvegreen, customColCurveblue, customColCurve2, customColCurve3, dummy, dummy, CAMBrightCurveJ, CAMBrightCurveQ, CAMMean, 0, 1, true, d, dj, yb, 1);
        }

        delete cieView;
        cieView = nullptr;
    }

    /*
     * gamut: the final working-space gamut compression and gain. Extracted
     * from stage_finish() unchanged.
     */
    void stage_gamut()
    {
        stage::ScopedProfile node_profile("gamut");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        bool exec = params.icm.wgamut != ColorManagementParams::Wwgamut::NONE  || params.icm.wgamgain != 0.f;

        if (params.icm.workingTRC != ColorManagementParams::WorkingTrc::NONE && params.icm.trcExp  && exec) {

            /*
             * The device runs the whole block in place. A refusal or a failed
             * launch falls through to the host block below unchanged, so the
             * host never starts from a partially written Lab buffer: the
             * launcher uploads, runs and reads the whole image back, and on
             * any failure it returns before the readback.
             */
            if (stage::node_device("gamut") == stage::StageNodeDevice::OpenCL
                    && run_gamut_stage(params.icm, *labView)) {
                return;
            }

            //compression gamut and gain at the end of process
            const int GW = labView->W;
            const int GH = labView->H;
            TMatrix wprof = ICCStore::getInstance()->workingSpaceMatrix(params.icm.workingProfile);
                const double wp[3][3] = {
                    {wprof[0][0], wprof[0][1], wprof[0][2]},
                    {wprof[1][0], wprof[1][1], wprof[1][2]},
                    {wprof[2][0], wprof[2][1], wprof[2][2]}
                };
            TMatrix wiprof = ICCStore::getInstance()->workingSpaceInverseMatrix(params.icm.workingProfile);
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
                        Color::Lab2XYZ(labView->L[i][j], labView->a[i][j], labView->b[i][j] , X, Y, Z);
                        Color::xyz2rgb(X, Y, Z, provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), wip);
                    }
                }

                const float gainev = pow_F(2.f, (float) params.icm.wgamgain);
                if (params.icm.wgamgain != 0.f) {//Final gain in Ev
           
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
                if (params.icm.wgamut != ColorManagementParams::Wwgamut::NONE) {
                    ipf.gamutcompr(provcomp.get(), provcomp.get(), beginend, powe, nbsegam, mac, mac0, mac1, mac2);
                }

#ifdef _OPENMP
        #   pragma omp parallel for
#endif
                for (int i = 0; i < GH; ++i){
                    for (int j = 0; j < GW; ++j) {
                        float x, y, z = 0.f;
                        Color::rgbxyz (provcomp->r(i, j), provcomp->g(i, j), provcomp->b(i, j), x, y, z, wp);
                        Color::XYZ2Lab(x, y, z, labView->L[i][j], labView->a[i][j], labView->b[i][j]);
                    }
                }
        }


        // end tile processing...???
        //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
        //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
    }

    /*
     * crop: the crop rectangle, the framing geometry the resize depends on,
     * the decision to crop and resize in Lab, and the Lab-space crop copy.
     * Extracted from stage_finish() unchanged.
     */
    void stage_crop()
    {
        stage::ScopedProfile node_profile("crop");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        if (pl) {
            pl->setProgress(0.60);
        }

        // crop and convert to rgb16
        cx = 0;
        cy = 0;
        cw = labView->W;
        ch = labView->H;
        if (params.crop.enabled) {
            cx = params.crop.x;
            cy = params.crop.y;
            cw = params.crop.w;
            ch = params.crop.h;
        }

        ImProcFunctions::FramingArgs framingArgs;
        framingArgs.params = &params;
        framingArgs.cropWidth = cw;
        framingArgs.cropHeight = ch;
        {
            int imw, imh;
            double tmpScale = ipf.resizeScale(&params, fw, fh, imw, imh);
            framingArgs.resizeWidth = imw;
            framingArgs.resizeHeight = imh;
            framingArgs.resizeScale = tmpScale;

            // If upscaling is not permitted, keep original sizing
            if ((cw < imw || ch < imh) && !params.resize.allowUpscaling) {
                framingArgs.resizeWidth = cw;
                framingArgs.resizeHeight = ch;
                framingArgs.resizeScale = 1.0;
            }
        }

        // If framing is not enabled, resize values simply pass through to output
        // The geometry is consumed by crop, resize, output.convert and
        // framing, so it lives on the processor rather than in a local.
        framingData = ipf.framing(framingArgs);
        if (settings->verbose) {
            printf("Framing Parameters (enabled=%s)\n", framingData.enabled ? "yes" : "no");
            printf("  Crop: w=%d h=%d\n", cw, ch);
            printf("  Original resize: w=%d h=%d s=%f\n",
                   framingArgs.resizeWidth, framingArgs.resizeHeight, framingArgs.resizeScale);
            printf("  Framed image size: w=%d h=%d s=%f\n",
                   framingData.imgWidth, framingData.imgHeight, framingData.scale);
            printf("  Total size: w=%d h=%d\n",
                   framingData.framedWidth, framingData.framedHeight);
        }

        labResize = params.resize.enabled && params.resize.method != "Nearest" &&
            (framingData.scale != 1.0 || params.prsharpening.enabled || framingData.enabled);

        if (params.crop.enabled) {
            if (labResize) { // crop lab data
                LabImage *tmplab = new LabImage(cw, ch);

                const std::size_t rowBytes =
                    static_cast<std::size_t>(cw) * sizeof(float);
                for (int row = 0; row < ch; ++row) {
                    std::memcpy(tmplab->L[row], labView->L[row + cy] + cx,
                                rowBytes);
                    std::memcpy(tmplab->a[row], labView->a[row + cy] + cx,
                                rowBytes);
                    std::memcpy(tmplab->b[row], labView->b[row + cy] + cx,
                                rowBytes);
                }

                delete labView;
                labView = tmplab;
                cx = 0;
                cy = 0;
            }
        }
    }

    /*
     * resize: the Lab resize when the crop survives as a Lab image. When the
     * output resize uses Nearest, the resize runs on the output RGB buffer
     * instead and is folded into output.convert; see STAGE_RUNTIME.md.
     */
    void stage_resize()
    {
        stage::ScopedProfile node_profile("resize");

        ImProcFunctions &ipf = * (ipf_p.get());

        if (labResize) { // resize lab data
            stage::ScopedProfile profile("finish.resize");
            int imw = framingData.imgWidth;
            int imh = framingData.imgHeight;
            if (labView->W != imw || labView->H != imh) {
                // resize image
                LabImage* resized = new LabImage(imw, imh);
                ipf.Lanczos(labView, resized, framingData.scale);
                delete labView;
                labView = resized;
            }

            cw = labView->W;
            ch = labView->H;
        }
    }

    /*
     * output.sharpen: the post-resize sharpening pass. Extracted from
     * stage_finish() unchanged.
     */
    void stage_output_sharpen()
    {
        stage::ScopedProfile node_profile("output.sharpen");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        if (labResize && params.prsharpening.enabled) {
            for (int i = 0; i < ch; i++) {
                for (int j = 0; j < cw; j++) {
                    labView->L[i][j] = labView->L[i][j] < 0.f ? 0.f : labView->L[i][j];
                }
            }

            ipf.sharpening(labView, params.prsharpening);
        }
    }

    /*
     * output.convert: convert the Lab image to the output RGB space and
     * profile, force black and white when the only colour work was a B&W
     * conversion, and run the output-space Nearest resize. Extracted from
     * stage_finish() unchanged.
     */
    void stage_output_convert()
    {
        stage::ScopedProfile node_profile("output.convert");

        procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

      //  bool bwonly = params.blackwhite.enabled && !params.colorToning.enabled && !autili && !butili && !params.colorappearance.enabled;
        bool bwonly = params.blackwhite.enabled && !params.colorToning.enabled && !autili && !butili && !usesCieCam02();

        ///////////// Custom output gamma has been removed, the user now has to create
        ///////////// a new output profile with the ICCProfileCreator

        // if Default gamma mode: we use the profile selected in the "Output profile" combobox;
        // gamma come from the selected profile, otherwise it comes from "Free gamma" tool

        const auto& saveFormat = App::get().options().saveFormat;
        const bool allowGpuOutput = !(saveFormat.format == "tif"
                                      && saveFormat.tiffFloat);
        {
            stage::ScopedProfile profile("finish.output");
            readyImg = ipf.lab2rgbOut(labView, cx, cy, cw, ch,
                                      params.icm, allowGpuOutput);
        }

        if (settings->verbose) {
            printf("Output profile_: \"%s\"\n", params.icm.outputProfile.c_str());
        }

        delete labView;
        labView = nullptr;

        if (bwonly) { //force BW r=g=b
            if (settings->verbose) {
                printf("Force BW\n");
            }

            for (int ccw = 0; ccw < cw; ccw++) {
                for (int cch = 0; cch < ch; cch++) {
                    readyImg->r(cch, ccw) = readyImg->g(cch, ccw);
                    readyImg->b(cch, ccw) = readyImg->g(cch, ccw);
                }
            }
        }

        if (pl) {
            pl->setProgress(0.70);
        }

        // The Nearest-method output resize runs on the gamma-applied RGB
        // buffer, which only exists after lab2rgbOut(). The canonical resize
        // node runs before the conversion, so this pass is folded into
        // output.convert and recorded as a fold in STAGE_RUNTIME.md.
        if (framingData.scale != 1.0 && params.resize.method == "Nearest") {
            int imw = framingData.imgWidth;
            int imh = framingData.imgHeight;
            if (readyImg->getWidth() != imw || readyImg->getHeight() != imh) {
                // resize rgb data (gamma applied)
                Imagefloat* tempImage = new Imagefloat(imw, imh);
                ipf.resize(readyImg, tempImage, framingData.scale);
                delete readyImg;
                readyImg = tempImage;
            }
        }
    }

    /*
     * framing: draw the frame around the output image. Extracted from
     * stage_finish() unchanged.
     */
    void stage_framing()
    {
        stage::ScopedProfile node_profile("framing");

        const procparams::ProcParams& params = job->pparams;
        ImProcFunctions &ipf = * (ipf_p.get());

        if (framingData.enabled) {
            readyImg = ipf.drawFrame(readyImg, params.framing, framingData);
        }
    }

    /*
     * output.icc: attach metadata and the output ICC profile, release the
     * source image and the job, and hand back the finished image. Extracted
     * from stage_finish() unchanged.
     */
    Imagefloat *stage_output_icc()
    {
        stage::ScopedProfile node_profile("output.icc");

        const procparams::ProcParams& params = job->pparams;

        Exiv2Metadata info(imgsrc->getFileName());

        switch (params.metadata.mode) {
            case MetaDataParams::TUNNEL:
                readyImg->setMetadata(std::move(info));
                break;

            case MetaDataParams::EDIT:
                info.setExif(params.metadata.exif);
                info.setIptc(params.metadata.iptc);

                if (!(params.metadata.exifKeys.size() == 1 && params.metadata.exifKeys[0] == "*")) {
                    info.setExifKeys(&(params.metadata.exifKeys));
                }

                readyImg->setMetadata(std::move(info));
                break;

            default: // case MetaDataParams::STRIP
                // nothing to do
                break;
        }


        // Setting the output curve to readyImg
        // use the selected output profile if present, otherwise use LCMS2 profile generate by lab2rgb16 w/ gamma

        if (!params.icm.outputProfile.empty() && params.icm.outputProfile != ColorManagementParams::NoICMString) {

            // if ICCStore::getInstance()->getProfile send back an object, then ICCStore::getInstance()->getContent will do too
            cmsHPROFILE jprof = ICCStore::getInstance()->getProfile(params.icm.outputProfile);  //get outProfile

            if (jprof == nullptr) {
                if (settings->verbose) {
                    printf("\"%s\" ICC output profile not found!\n - use LCMS2 substitution\n", params.icm.outputProfile.c_str());
                }
            } else {
                if (settings->verbose) {
                    printf("Using \"%s\" output profile\n", params.icm.outputProfile.c_str());
                }

                ProfileContent pc = ICCStore::getInstance()->getContent(params.icm.outputProfile);
                readyImg->setOutputProfile(pc.getData());
            }
        } else {
            // No ICM
            readyImg->setOutputProfile({});
        }

//    t2.set();
//    if( settings->verbose )
//           printf("Total:- %d usec\n", t2.etime(t1));

        if (!job->initialImage) {
            initialImage->decreaseRef();
        }

        delete job;

        if (pl) {
            pl->setProgress(0.75);
        }

        /*  curve1.reset();curve2.reset();
            curve.reset();
            satcurve.reset();
            lhskcurve.reset();

            rCurve.reset();
            gCurve.reset();
            bCurve.reset();
            hist16.reset();
            hist16C.reset();
        */
        return readyImg;
    }

    void stage_early_resize()
    {
        // The fast pipeline has no outer stage timings, so time this stage here
        // to keep it comparable with export.transform and export.finish.
        stage::ScopedProfile profile("export.early_resize");

        procparams::ProcParams& params = job->pparams;
        //ImProcFunctions ipf (&params, true);
        ImProcFunctions &ipf = * (ipf_p.get());

        int imw, imh;
        double scale_factor = ipf.resizeScale(&params, fw, fh, imw, imh);

        std::unique_ptr<LabImage> tmplab;

        if (params.crop.enabled) {
            // The crop rectangle is the only part of the frame that survives
            // the resize, so convert that rectangle instead of the whole
            // frame and copying it afterwards. stage_init() has already
            // clamped the rectangle to the frame.
            const int cx = params.crop.x;
            const int cy = params.crop.y;
            const int cw = params.crop.w;
            const int ch = params.crop.h;
            assert(cx >= 0 && cy >= 0);
            assert(cx + cw <= fw && cy + ch <= fh);

            tmplab.reset(new LabImage(cw, ch));
            rgbToLabRegion(*baseImg, *tmplab, cx, cy, params.icm.workingProfile);
        } else {
            tmplab.reset(new LabImage(fw, fh));
            ipf.rgb2lab(*baseImg, *tmplab, params.icm.workingProfile);
        }

        assert(params.resize.enabled);

        // resize image
        if (params.resize.allowUpscaling || (imw <= fw && imh <= fh)) {
            std::unique_ptr<LabImage> resized(new LabImage(imw, imh));
            ipf.Lanczos(tmplab.get(), resized.get(), scale_factor);
            tmplab = std::move(resized);
        }

        adjust_procparams(scale_factor);

        fw = imw;
        fh = imh;

        delete baseImg;
        baseImg = new Imagefloat(fw, fh);
        ipf.lab2rgb(*tmplab, *baseImg, params.icm.workingProfile);
    }

    void adjust_procparams(double scale_factor)
    {
        procparams::ProcParams &params = job->pparams;
        procparams::ProcParams defaultparams;

        params.resize.enabled = false;
        params.crop.enabled = false;

        if (params.prsharpening.enabled) {
            params.sharpening = params.prsharpening;
        } else {
            params.sharpening.radius *= scale_factor;
            params.sharpening.deconvradius *= scale_factor;
        }

        params.impulseDenoise.thresh *= scale_factor;

        if (scale_factor < 0.5) {
            params.impulseDenoise.enabled = false;
        }

        params.wavelet.strength *= scale_factor;
        double noise_factor = (1.0 - scale_factor);
        params.dirpyrDenoise.luma *= noise_factor; // * scale_factor;
        //params.dirpyrDenoise.Ldetail += (100 - params.dirpyrDenoise.Ldetail) * scale_factor;
        auto &lcurve = params.dirpyrDenoise.lcurve;

        for (size_t i = 2; i < lcurve.size(); i += 4) {
            lcurve[i] *= min(noise_factor /* * scale_factor*/, 1.0);
        }

        noiseLCurve.Set(lcurve);
        const char *medmethods[] = { "soft", "33", "55soft", "55", "77", "99" };

        if (params.dirpyrDenoise.median) {
            auto &key = params.dirpyrDenoise.methodmed == "RGB" ? params.dirpyrDenoise.rgbmethod : params.dirpyrDenoise.medmethod;

            for (int i = 1; i < int (sizeof(medmethods) / sizeof(const char *)); ++i) {
                if (key == medmethods[i]) {
                    int j = i - int (1.0 / scale_factor);

                    if (j < 0) {
                        params.dirpyrDenoise.median = false;
                    } else {
                        key = medmethods[j];
                    }

                    break;
                }
            }
        }

        params.epd.scale *= scale_factor;
        //params.epd.edgeStopping *= scale_factor;

        const double dirpyreq_scale = min(scale_factor * 1.5, 1.0);

        for (int i = 0; i < 6; ++i) {
            adjust_radius(defaultparams.dirpyrequalizer.mult[i], dirpyreq_scale,
                          params.dirpyrequalizer.mult[i]);
        }

        params.dirpyrequalizer.threshold *= scale_factor;

        adjust_radius(defaultparams.defringe.radius, scale_factor,
                      params.defringe.radius);
        params.sh.radius *= scale_factor;
        params.localContrast.radius *= scale_factor;

        if (params.raw.xtranssensor.method == procparams::RAWParams::XTransSensor::getMethodString(procparams::RAWParams::XTransSensor::Method::THREE_PASS)) {
            params.raw.xtranssensor.method = procparams::RAWParams::XTransSensor::getMethodString(procparams::RAWParams::XTransSensor::Method::ONE_PASS);
        }

        if (params.raw.bayersensor.method == procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::PIXELSHIFT)) {
            params.raw.bayersensor.method = procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::RCD);
        }

        // Use Rcd instead of Amaze for fast export
        if (params.raw.bayersensor.method == procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::AMAZE)) {
            params.raw.bayersensor.method = procparams::RAWParams::BayerSensor::getMethodString(procparams::RAWParams::BayerSensor::Method::RCD);
        }
    }

private:
    ProcessingJobImpl* job;
    int& errorCode;
    ProgressListener* pl;
    bool flush;

    // internal state
    std::unique_ptr<ImProcFunctions> ipf_p;
    InitialImage *initialImage;
    ImageSource *imgsrc;
    int fw;
    int fh;

    int tr;
    PreviewProps pp;

    NoiseCurve noiseLCurve;
    NoiseCurve noiseCCurve;
    Imagefloat *calclum;
    float autoNR;
    float autoNRmax;
    int tilesize;
    int overlap;

    float *ch_M;
    float *max_r;
    float *max_b;
    float *min_b;
    float *min_r;
    float *lumL;
    float *chromC;
    float *ry;
    float *sk;
    float *pcsk;

    double expcomp;
    int bright;
    int contr;
    int black;
    int hlcompr;
    int hlcomprthresh;

    ColorTemp currWB;
    Imagefloat *baseImg;
    LabImage* labView;

    /* Crop rectangle consumed by the output nodes; see stage_crop(). */
    int cx;
    int cy;
    int cw;
    int ch;
    /*
     * True when the Lab image is cropped and resized before the output
     * conversion. False when resize is disabled or uses Nearest, in which case
     * the resize runs on the output RGB buffer.
     */
    bool labResize;
    /* Framing geometry shared by crop, resize, output.convert and framing. */
    ImProcFunctions::FramingData framingData;
    /* Output image produced by output.convert and completed by output.icc. */
    Imagefloat* readyImg;

    LUTu hist16;

    LUTf curve1;
    LUTf curve2;
    LUTf curve;
    LUTf satcurve;
    LUTf lhskcurve;
    LUTf lumacurve;
    LUTf clcurve;
    LUTf clToningcurve;
    LUTf cl2Toningcurve;
    LUTf wavclCurve;

    LUTf rCurve;
    LUTf gCurve;
    LUTf bCurve;
    LUTu dummy;

    ToneCurve customToneCurve1, customToneCurve2;
    ColorGradientCurve ctColorCurve;
    OpacityCurve ctOpacityCurve;
    ColorAppearance customColCurve1, customColCurvered, customColCurvegreen, customColCurveblue, customColCurve2, customColCurve3 ;
    ToneCurve customToneCurvebw1;
    ToneCurve customToneCurvebw2;

    bool autili, butili;
};

} // namespace


IImagefloat* processImage(ProcessingJob* pjob, int& errorCode, ProgressListener* pl, bool flush)
{
    ImageProcessor proc(pjob, errorCode, pl, flush);
    return proc();
}

void batchProcessingThread(ProcessingJob* job, BatchProcessingListener* bpl)
{
    ProcessingJob* currentJob = job;

    while (currentJob) {
        int errorCode;
        IImagefloat* img = processImage(currentJob, errorCode, bpl, true);

        if (errorCode) {
            bpl->error(M("MAIN_MSG_CANNOTLOAD"));
            currentJob = nullptr;
        } else {
            try {
                currentJob = bpl->imageReady(img);
            } catch (Glib::Exception& ex) {
                bpl->error(ex.what());
                currentJob = nullptr;
            }
        }
    }
}

void startBatchProcessing(ProcessingJob* job, BatchProcessingListener* bpl)
{

    if (bpl) {
        Glib::Thread::create(sigc::bind(sigc::ptr_fun(batchProcessingThread), job, bpl), 0, true, true, Glib::THREAD_PRIORITY_LOW);
    }

}

}
