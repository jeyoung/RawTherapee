/*
 *  This file is part of RawTherapee.
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

/*
 * Typed work-order derivation.
 *
 * The unit is a predicate table over WorkItem; it owns no state, executes
 * nothing and allocates nothing beyond the returned value and a diagnostic
 * string. See stage_work.h for the per-item contract, the gate each predicate
 * replaces and the facts that are deliberately not read.
 *
 * Two rules keep a caller from misreading a first pass:
 *
 * - a missing history is not an empty history. No previous state or
 *   fingerprints means every parameter delta counts, which is the destructive
 *   set; the legacy first pass runs `ALL` for the same reason.
 * - a missing current state, fingerprint table or parameter set is a
 *   programming error. The assertion fires in a debug build and the answer is
 *   the whole path-applicable set, never an empty order.
 */

#include "stage_work.h"

#include "procparams.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>

namespace rtengine::stage
{

namespace
{

using procparams::ProcParams;

/*
 * History is usable only when both fingerprint tables describe the same path
 * in the same order. A caller that changed path or dropped ids must not have
 * unrelated values compared field by field.
 */
bool usableHistory(const WorkInputs& inputs)
{
    if (inputs.previousState == nullptr || inputs.currentState == nullptr
        || inputs.previousFingerprints == nullptr
        || inputs.currentFingerprints == nullptr) {
        return false;
    }

    const BlockFingerprints& previous = *inputs.previousFingerprints;
    const BlockFingerprints& current = *inputs.currentFingerprints;
    return !current.empty() && previous.ids == current.ids
        && previous.values.size() == current.values.size();
}

/*
 * Whether the canonical block `id` changed.
 *
 * Without usable history every block counts as changed, because the pass has
 * no basis for trusting any cached output. An id that the path does not carry
 * is a programming error: assert and answer "changed" so a release build
 * fails closed.
 */
bool blockChanged(const WorkInputs& inputs, const char* id)
{
    if (!usableHistory(inputs)) {
        return true;
    }

    const BlockFingerprints& current = *inputs.currentFingerprints;
    const BlockFingerprints& previous = *inputs.previousFingerprints;
    for (size_t i = 0; i < current.ids.size(); ++i) {
        if (current.ids[i] == id) {
            return previous.values[i] != current.values[i];
        }
    }

    assert(false && "work order asks for a block outside the path");
    return true;
}

/*
 * The source reload identity.
 *
 * `rgbSourceModified` is deliberately excluded: the preprocess and base gates
 * do not consult it, and the demosaic gate reads it through its own
 * highlight-recovery condition. A bare in-place RGB modification is therefore
 * not a reload, while a generation bump or a change of the source geometry is.
 */
bool sourceReloaded(const RenderStateKey& previous,
                    const RenderStateKey& current)
{
    return std::tie(previous.sourceGeneration, previous.sourceRotateDegree,
                    previous.sourceSensorType, previous.sourceFullWidth,
                    previous.sourceFullHeight, previous.sourceFrameCount)
           != std::tie(current.sourceGeneration, current.sourceRotateDegree,
                       current.sourceSensorType, current.sourceFullWidth,
                       current.sourceFullHeight, current.sourceFrameCount);
}

/*
 * Delta of one ProcParams member.
 *
 * `BlockFingerprints` keeps the raw.base families merged in one hash, so a
 * retinex or spot delta cannot be told apart from a white-balance delta. The
 * previous parameter set is carried for exactly this question. A missing
 * history or previous set answers "changed", which is the conservative
 * reading and matches the legacy first pass.
 */
template <typename Member>
bool paramFamilyChanged(const WorkInputs& inputs,
                        Member procparams::ProcParams::*member)
{
    if (!usableHistory(inputs) || inputs.previousParams == nullptr
        || inputs.params == nullptr) {
        return true;
    }

    return (inputs.previousParams->*member) != (inputs.params->*member);
}

/*
 * Whether colour management changed in a way that rebuilds the base.
 *
 * The output profile, intent and black-point compensation are monitor-sink
 * inputs (`EvOProfile` maps to M_MONITOR), while the working space is read by
 * getImage(). Comparing the whole `icm` member would name the base for an
 * output-profile edit that the legacy gate does not rebuild.
 */
bool baseIcmChanged(const WorkInputs& inputs)
{
    if (!usableHistory(inputs) || inputs.previousParams == nullptr
        || inputs.params == nullptr) {
        return true;
    }

    procparams::ColorManagementParams previous = inputs.previousParams->icm;
    const procparams::ColorManagementParams& current = inputs.params->icm;
    previous.outputProfile = current.outputProfile;
    previous.outputIntent = current.outputIntent;
    previous.outputBPC = current.outputBPC;
    return previous != current;
}

bool isColourHlrMethod(const ProcParams& params)
{
    return params.toneCurve.method == "Color"
        || params.toneCurve.method == "Coloropp";
}

/*
 * The colour highlight-recovery condition of rawSourceWorkRuns()
 * (improccoordinator.cc:1362): a modified RGB source is re-demosaiced when the
 * recovery method and the source colour space disagree, in either direction.
 */
bool hlrDemandsDemosaic(const ProcParams& params,
                        const RenderStateKey& state)
{
    if (!state.rgbSourceModified) {
        return false;
    }

    const bool isColor = isColourHlrMethod(params);
    return (params.toneCurve.hrenabled && !isColor)
        || (!params.toneCurve.hrenabled && isColor);
}

void setAll(WorkOrder& order, std::initializer_list<WorkItem> items)
{
    for (WorkItem item : items) {
        order.set(item);
    }
}

/*
 * Whether a request carries the destructive raw reload bits.
 *
 * FullPreview and FullCrop stand for `M_INIT | M_LINDENOISE | M_HDR`, the
 * legacy reload gate. HighQuality stands for `M_HIGHQUAL`, which asks for the
 * accurate chain but is not part of that gate: the preview maps a bare
 * M_HIGHQUAL request to it (imagearea.cc:809) and the crop does the same, so a
 * HighQuality-only pass must not be treated as a raw reload.
 */
bool requestReloadsRaw(RequestKind kind)
{
    return kind == RequestKind::FullCrop || kind == RequestKind::FullPreview;
}

} // namespace

const char* workItemName(WorkItem item)
{
    switch (item) {
    case WorkItem::sourceSelect:
        return "sourceSelect";
    case WorkItem::rawPreprocess:
        return "rawPreprocess";
    case WorkItem::rawDemosaic:
        return "rawDemosaic";
    case WorkItem::rawBaseRebuild:
        return "rawBaseRebuild";
    case WorkItem::retinexPrepare:
        return "retinexPrepare";
    case WorkItem::spotApply:
        return "spotApply";
    case WorkItem::spotRestore:
        return "spotRestore";
    case WorkItem::analysisRun:
        return "analysisRun";
    case WorkItem::denoiseRun:
        return "denoiseRun";
    case WorkItem::Count:
        break;
    }

    return "unknown";
}

bool WorkOrder::has(WorkItem item) const
{
    const size_t index = static_cast<size_t>(item);
    assert(index < workItemCount && "WorkItem out of range");
    if (index >= workItemCount) {
        return false;
    }

    const uint64_t mask = uint64_t{1} << (index % wordBits);
    return (words_[index / wordBits] & mask) != 0;
}

void WorkOrder::set(WorkItem item)
{
    const size_t index = static_cast<size_t>(item);
    assert(index < workItemCount && "cannot set WorkItem::Count");

    if (index < workItemCount) {
        words_[index / wordBits] |= uint64_t{1} << (index % wordBits);
    }
}

void WorkOrder::clear(WorkItem item)
{
    const size_t index = static_cast<size_t>(item);
    assert(index < workItemCount && "cannot clear WorkItem::Count");

    if (index < workItemCount) {
        words_[index / wordBits] &= ~(uint64_t{1} << (index % wordBits));
    }
}

size_t WorkOrder::count() const
{
    size_t total = 0;
    for (size_t i = 0; i < workItemCount; ++i) {
        if (has(static_cast<WorkItem>(i))) {
            ++total;
        }
    }
    return total;
}

bool WorkOrder::empty() const
{
    return count() == 0;
}

bool WorkOrder::operator==(const WorkOrder& other) const
{
    return words_ == other.words_;
}

bool WorkOrder::operator!=(const WorkOrder& other) const
{
    return !(*this == other);
}

std::string workOrderName(const WorkOrder& order)
{
    std::string text = "[";
    bool first = true;
    for (size_t i = 0; i < workItemCount; ++i) {
        const WorkItem item = static_cast<WorkItem>(i);
        if (!order.has(item)) {
            continue;
        }
        if (!first) {
            text += ' ';
        }
        text += workItemName(item);
        first = false;
    }
    text += ']';
    return text;
}

WorkOrder derive_preview_work(const WorkInputs& inputs)
{
    assert(inputs.path == StagePath::Preview
           && "derive_preview_work called for another path");
    assert(inputs.currentState != nullptr
           && "preview work order needs a current render state");
    assert(inputs.currentFingerprints != nullptr
           && "preview work order needs current fingerprints");
    assert(inputs.params != nullptr
           && "preview work order needs the current parameters");

    WorkOrder order;

    /*
     * Fail closed on a violated precondition: every item the preview path can
     * run. denoiseRun is absent because the preview has no denoise body. The
     * null parameters case is included even though the assert above names it,
     * because a release build must not derive an empty order from a missing
     * parameter set.
     */
    if (inputs.currentState == nullptr || inputs.currentFingerprints == nullptr
        || inputs.params == nullptr) {
        setAll(order, {WorkItem::sourceSelect, WorkItem::rawPreprocess,
                       WorkItem::rawDemosaic, WorkItem::rawBaseRebuild,
                       WorkItem::retinexPrepare, WorkItem::spotApply,
                       WorkItem::spotRestore, WorkItem::analysisRun});
        return order;
    }

    assert(inputs.requestKind == inputs.currentState->requestKind
           && "WorkInputs::requestKind must match the current render state");

    const RenderStateKey& current = *inputs.currentState;
    const bool history = usableHistory(inputs);
    const RenderStateKey& previous =
        history ? *inputs.previousState : current;

    const ProcParams& params = *inputs.params;
    const bool retinexEnabled = params.retinex.enabled;
    const bool hlr = hlrDemandsDemosaic(params, current);

    /*
     * A change of the high-detail decision only schedules the raw core when
     * the work for the new decision is still pending; that fact is carried by
     * the high-detail fields and read by `hdPre`/`hdRaw` below. A bare
     * decision change is a mask-level request (the caller sends ALL on a zoom
     * transition), not a parameter delta, so naming it here would create a
     * typed-only run against a mask that did not carry it.
     */

    const bool sourceReload = history && sourceReloaded(previous, current);

    /*
     * A reload request is absolute, not a delta. The legacy request bits are
     * M_INIT, M_LINDENOISE and M_HDR, which the caller folds into FullPreview
     * or FullCrop. M_HIGHQUAL is a separate request: it asks for the accurate
     * chain, and the legacy base gate does not consult it, so HighQuality is
     * deliberately not a raw reload here.
     */
    const bool rawReload = requestReloadsRaw(inputs.requestKind);

    const bool selectChanged = blockChanged(inputs, "source.select");
    const bool preprocessBlockChanged = blockChanged(inputs, "raw.preprocess");
    const bool demosaicBlockChanged = blockChanged(inputs, "raw.demosaic");

    const bool hdPrePending = current.highDetailNeeded
        && !current.highDetailPreprocessComputed;
    const bool hdRawPending = current.highDetailNeeded
        && !current.highDetailRawComputed;
    /*
     * The high-detail decision also chooses the demosaic mode: below 100% the
     * caller substitutes the FAST method in the pass's RAWParams, while at
     * 100% the configured method runs. Dropping the decision therefore needs
     * a fresh demosaic even though no ProcParams field moved, and every
     * crossing queues a full request that carries M_RAW. The pending-work
     * fields above only cover the direction that gains high detail.
     */
    const bool highDetailModeChanged =
        history && previous.highDetailNeeded != current.highDetailNeeded;

    /*
     * source.select and raw.preprocess move together: rawWorkRuns() feeds
     * both. A selected-frame change (`raw.bayersensor.imageNum`) is a
     * DARKFRAME event, which also sets M_PREPROC and M_RAW, so it regenerates
     * the source selection and the preprocessed planes. A rebuild request is
     * not a disjunct: RequestKind folds M_INIT/M_LINDENOISE/M_HDR into
     * FullPreview/FullCrop, and those bits rebuild the base without selecting
     * or preprocessing the source.
     */
    const bool source = selectChanged || sourceReload;
    const bool preprocess = preprocessBlockChanged || source || hdPrePending;

    /*
     * The raw.base families that are also read by the demosaic body and the
     * retinex preparation. Computed once: `wbChanged` feeds both the base and
     * the Color/Coloropp augmentation, `icmChanged` feeds the base and the
     * retinex preparation (the working profile is a real input there).
     */
    const bool wbChanged = paramFamilyChanged(inputs, &ProcParams::wb);
    const bool icmChanged = baseIcmChanged(inputs);

    /*
     * The caller's Color/Coloropp white-balance augmentation is a mask fact:
     * `M_WB` plus the recovery method, folded by the caller before the pass
     * (improccoordinator.cc:4610-4618). getImage() applies the opposed
     * recovery with the current white-balance multipliers and marks the planes
     * recovered, so that request needs fresh demosaic planes. The `wb` family
     * delta alone is not enough: the same delta can arrive in a pass whose mask
     * does not carry M_WB (a caller re-sending a value the previous pass
     * normalised), and naming the body then would be a typed-only run.
     */
    const bool hlrWbDemosaic = inputs.colorHlrWbRequest
        && params.toneCurve.hrenabled
        && params.toneCurve.method == "Coloropp";

    /*
     * A recovery method, blend or threshold delta can only take effect on
     * fresh planes once getImage() has marked the current planes recovered
     * (`rgbSourceModified`). When the planes carry no recovery output yet,
     * getImage() applies the new setting to them and a fresh demosaic would
     * change nothing, so that case stays legacy-only redundant work.
     */
    const bool hlrConfigChanged = history && inputs.previousParams != nullptr
        && current.rgbSourceModified && params.toneCurve.hrenabled
        && isColourHlrMethod(params)
        && (inputs.previousParams->toneCurve.method != params.toneCurve.method
            || inputs.previousParams->toneCurve.hlbl != params.toneCurve.hlbl
            || inputs.previousParams->toneCurve.hlth != params.toneCurve.hlth);

    /*
     * rawSourceWorkRuns(): the sensor demosaic fields, capture sharpening, a
     * pending high-detail demosaic, the colour highlight-recovery condition of
     * a modified RGB source, the retinex plane work, and the forward edges from
     * a selected frame or a preprocessed source. Every DARKFRAME/FLATFIELD
     * event sets M_RAW as well as M_PREPROC, so a preprocess change also runs
     * the demosaic gate.
     *
     * Capture sharpening modifies the demosaiced planes in place, so the body
     * must run not only when the tool becomes enabled but also when it becomes
     * disabled: only a fresh demosaic removes the sharpening the previous pass
     * left in the planes. The legacy DEMOSAIC event carries M_RAW in both
     * directions. A pdsharpening-only delta while the tool was and is disabled
     * still leaves the body un-named, because the planes are unchanged; that
     * legacy-only remainder is redundant work.
     *
     * `pdSharpeningChanged` separates the capture-sharpening family from the
     * sensor fields. Only claim a pdsharpening-only delta when the previous
     * parameter set is actually available: without it the family comparison is
     * conservatively true, which would otherwise hide a sensor demosaic delta
     * carried by the same block fingerprint.
     */
    const bool pdSharpeningChanged = history && inputs.previousParams != nullptr
        && paramFamilyChanged(inputs, &ProcParams::pdsharpening);
    const bool previousCaptureSharpening =
        history && inputs.previousParams != nullptr
        && inputs.previousParams->pdsharpening.enabled;
    const bool captureSharpening = pdSharpeningChanged
        && (params.pdsharpening.enabled || previousCaptureSharpening);
    const bool sensorDemosaicChanged =
        demosaicBlockChanged && !pdSharpeningChanged;

    /*
     * Retinex buffer preparation lives inside the raw.demosaic body, not in
     * the retinex preparation item: `retinexPrepareBuffers()` runs before
     * getImage() when the planes are regenerated, and `retinex()` in raw.base
     * consumes those buffers. The body must therefore run when retinex is
     * newly enabled (there are no buffers yet), when it is disabled (the
     * current planes still carry the retinex output and only a fresh demosaic
     * removes it), and when the working profile moves while it is enabled
     * (the buffers are converted with the working-space matrix). The legacy
     * DEMOSAIC event carries M_RAW in each of those cases. A retinex strength
     * edit carries M_RETINEX instead and reuses the existing buffers, so it
     * must not name the demosaic body.
     */
    const bool previousRetinexEnabled =
        history && inputs.previousParams != nullptr
        && inputs.previousParams->retinex.enabled;
    const bool retinexPlanes = retinexEnabled
        ? (inputs.previousParams == nullptr || !previousRetinexEnabled
           || icmChanged)
        : (inputs.previousParams != nullptr && previousRetinexEnabled);

    const bool demosaic = sensorDemosaicChanged || captureSharpening || source
        || preprocessBlockChanged || hdRawPending || highDetailModeChanged
        || hlr || hlrWbDemosaic || hlrConfigChanged || retinexPlanes;

    /*
     * baseRebuildRuns(): the raw.base families that actually rebuild, plus the
     * forward edges that regenerate the planes the base reads. The raw.base
     * block fingerprint is not used: it merges spot and retinex with the
     * rebuilding families. A preprocess or a demosaic regenerates the planes
     * the base consumes (the forward-edge rule of stage_dirty_blocks.h,
     * close_dirty_blocks). `dirpyrDenoise` is one of the families because the
     * legacy base gate includes M_LINDENOISE. A HighQuality-only request is not
     * a rebuild: `rawReload` excludes it.
     */
    const bool base = preprocess || demosaic || wbChanged
        || paramFamilyChanged(inputs, &ProcParams::toneCurve)
        || paramFamilyChanged(inputs, &ProcParams::dirpyrDenoise)
        || paramFamilyChanged(inputs, &ProcParams::filmNegative)
        || paramFamilyChanged(inputs, &ProcParams::retinex)
        || paramFamilyChanged(inputs, &ProcParams::cg)
        || icmChanged
        || rawReload;

    /*
     * A spot delta does not rebuild the base: the legacy M_SPOT bit only
     * enters the spot gates inside raw.base and the buffer restore. The
     * request fact `spotPending` already carries the enabled/entries test.
     */
    const bool spotsConfigured =
        params.spot.enabled && !params.spot.entries.empty();
    const bool spotChanged = spotsConfigured
        && paramFamilyChanged(inputs, &ProcParams::spot);
    const bool spotApply = current.spotPending || spotChanged;

    if (preprocess) {
        order.set(WorkItem::sourceSelect);
        order.set(WorkItem::rawPreprocess);
    }

    if (demosaic) {
        order.set(WorkItem::rawDemosaic);
    }

    if (base) {
        order.set(WorkItem::rawBaseRebuild);
        /* analysis is folded into raw.base on the preview path. */
        order.set(WorkItem::analysisRun);
    }

    /*
     * Retinex rewrites the shared raw planes before getImage(). It is named
     * when its own parameters move, when a step that regenerates those planes
     * runs, or when the working profile moves: the preparation converts the
     * planes with the working-space matrix and back, so a working-profile delta
     * is a real input even with unchanged planes. A white-balance-only M_INIT
     * regenerates neither and is not read by the preparation, so it stays
     * legacy-only redundant work.
     */
    if (retinexEnabled
        && (paramFamilyChanged(inputs, &ProcParams::retinex) || preprocess
            || demosaic || icmChanged)) {
        order.set(WorkItem::retinexPrepare);
    }

    /*
     * A base rebuild removes the spots from the freshly rebuilt image even
     * when their parameters did not move; the request fact covers that case.
     * The buffer path applies them when there is no base rebuild.
     */
    if (spotApply) {
        order.set(WorkItem::spotApply);
    }

    /*
     * The direct base path applies the spots itself, so the copy-back is only
     * needed when the buffer path ran or a buffer is still live. A live buffer
     * is also named when the tool is now disabled: the buffer-maintenance site
     * clears it, and the copy-back then finds no buffer. Naming that state is
     * what lets the site stop reading M_SPOT.
     */
    if (current.spotBufferLive || (spotApply && !base)) {
        order.set(WorkItem::spotRestore);
    }

    return order;
}

WorkOrder derive_crop_work(const WorkInputs& inputs)
{
    assert(inputs.path == StagePath::Crop
           && "derive_crop_work called for another path");
    assert(inputs.currentState != nullptr
           && "crop work order needs a current render state");
    assert(inputs.currentFingerprints != nullptr
           && "crop work order needs current fingerprints");
    assert(inputs.params != nullptr
           && "crop work order needs the current parameters");

    WorkOrder order;

    /*
     * Fail closed on a violated precondition: every item the crop path can
     * run. The crop never preprocesses, demosaics or prepares retinex. The
     * null parameters case is included even though the assert above names it,
     * because a release build must not derive an empty order from a missing
     * parameter set.
     */
    if (inputs.currentState == nullptr || inputs.currentFingerprints == nullptr
        || inputs.params == nullptr) {
        setAll(order, {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                       WorkItem::spotApply, WorkItem::spotRestore,
                       WorkItem::analysisRun, WorkItem::denoiseRun});
        return order;
    }

    assert(inputs.requestKind == inputs.currentState->requestKind
           && "WorkInputs::requestKind must match the current render state");

    const RenderStateKey& current = *inputs.currentState;
    const bool history = usableHistory(inputs);

    const ProcParams& params = *inputs.params;
    const bool denoiseEnabled = params.dirpyrDenoise.enabled;
    const bool spotsConfigured =
        params.spot.enabled && !params.spot.entries.empty();
    const bool spotChanged = spotsConfigured
        && paramFamilyChanged(inputs, &ProcParams::spot);

    /*
     * The destructive bodies share the reload request. The crop folds
     * `M_INIT | M_LINDENOISE | M_HDR` into RequestKind::FullCrop before it
     * builds the key; that fold is the only mask-derived fact the derivation
     * reads. A source reload or a high-detail transition reaches the crop
     * through the same bits, so it is not an independent disjunct here; the
     * dirty-block derivation models those render-state aspects separately.
     * HighQuality maps M_HIGHQUAL, which the reload request does not carry,
     * and is therefore not a reload. No history means the first update, which
     * runs the full destructive set.
     */
    const bool reload = !history || requestReloadsRaw(inputs.requestKind);

    if (reload) {
        order.set(WorkItem::sourceSelect);
        order.set(WorkItem::rawBaseRebuild);
        order.set(WorkItem::analysisRun);
    }

    if (reload && denoiseEnabled && current.cropSkip == 1) {
        order.set(WorkItem::denoiseRun);
    }

    /*
     * The direct base path applies the spots itself (M_SPOT inside the reload
     * branch); without a reload the spot buffer path runs and is then swapped
     * into place.
     */
    const bool spotApply = current.spotPending || spotChanged;

    if (spotApply) {
        order.set(WorkItem::spotApply);
    }

    if (current.spotBufferLive || (spotApply && !reload)) {
        order.set(WorkItem::spotRestore);
    }

    return order;
}

} // namespace rtengine::stage
