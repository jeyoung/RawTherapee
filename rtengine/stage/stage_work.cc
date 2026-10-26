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

void WorkShadow::probe(WorkItem item, bool legacyRuns)
{
    if (forced_) {
        return;
    }

    const size_t index = static_cast<size_t>(item);
    assert(index < workItemCount && "WorkShadow probe out of range");
    if (index >= workItemCount) {
        return;
    }

    probed_[index] = true;
    legacy_[index] = legacy_[index] || legacyRuns;
}

void WorkShadow::probeRawBaseNode(bool legacyRuns)
{
    if (forced_) {
        return;
    }

    nodeProbed_ = true;
    nodeLegacy_ = nodeLegacy_ || legacyRuns;
}

void WorkShadow::skipForcedPass()
{
    forced_ = true;
}

void WorkShadow::report(const char* path, const WorkOrder& order) const
{
    static std::atomic<uint64_t> nextPass{1};
    const uint64_t pass = nextPass.fetch_add(1, std::memory_order_relaxed);

    if (forced_) {
        std::fprintf(stderr,
                     "work-shadow %s pass=%llu typedOnly=0 legacyOnly=0 "
                     "agree=0 items=forced-no-history\n",
                     path, static_cast<unsigned long long>(pass));
        return;
    }

    std::string typedOnly;
    std::string legacyOnly;
    size_t typedCount = 0;
    size_t legacyCount = 0;
    size_t agree = 0;

    const auto classify = [&](const char* name, bool typed, bool legacy) {
        if (typed && !legacy) {
            if (!typedOnly.empty()) {
                typedOnly += ' ';
            }
            typedOnly += name;
            ++typedCount;
        } else if (!typed && legacy) {
            if (!legacyOnly.empty()) {
                legacyOnly += ' ';
            }
            legacyOnly += name;
            ++legacyCount;
        } else {
            ++agree;
        }
    };

    for (size_t i = 0; i < workItemCount; ++i) {
        if (!probed_[i]) {
            continue;
        }
        const WorkItem item = static_cast<WorkItem>(i);
        classify(workItemName(item), order.has(item), legacy_[i]);
    }

    if (nodeProbed_) {
        const bool nodeTyped = order.has(WorkItem::rawBaseRebuild)
            || order.has(WorkItem::retinexPrepare)
            || order.has(WorkItem::spotApply)
            || order.has(WorkItem::spotRestore)
            || order.has(WorkItem::analysisRun);
        classify("rawBaseNode", nodeTyped, nodeLegacy_);
    }

    std::string items;
    if (!typedOnly.empty()) {
        items += "typedOnly[" + typedOnly + "]";
    }
    if (!legacyOnly.empty()) {
        if (!items.empty()) {
            items += ' ';
        }
        items += "legacyOnly[" + legacyOnly + "]";
    }
    if (items.empty()) {
        items = "none";
    }

    std::fprintf(stderr,
                 "work-shadow %s pass=%llu typedOnly=%zu legacyOnly=%zu "
                 "agree=%zu items=%s\n",
                 path, static_cast<unsigned long long>(pass), typedCount,
                 legacyCount, agree, items.c_str());
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
     * run. denoiseRun is absent because the preview has no denoise body.
     */
    if (inputs.currentState == nullptr || inputs.currentFingerprints == nullptr) {
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

    const ProcParams* params = inputs.params;
    const bool retinexEnabled = params == nullptr || params->retinex.enabled;
    const bool hlr = params != nullptr && hlrDemandsDemosaic(*params, current);

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
     * A rebuild request is absolute, not a delta: FullPreview, FullCrop and
     * HighQuality all name a pass that rebuilds the path even when no input
     * moved. The legacy request bits are M_INIT, M_LINDENOISE and M_HDR for
     * the reload and M_HIGHQUAL for the accurate chain.
     */
    const bool rebuild = request_rebuilds_path(inputs.requestKind);

    /*
     * rawWorkRuns(): M_PREPROC or a high-detail preprocess that is still
     * pending. The source facts join because a reload re-selects the frame
     * before it preprocesses. A rebuild request is deliberately absent:
     * RequestKind folds M_INIT/M_LINDENOISE/M_HDR into FullPreview, and those
     * bits rebuild the base without selecting or preprocessing the source.
     */
    const bool preprocess = blockChanged(inputs, "raw.preprocess")
        || (current.highDetailNeeded && !current.highDetailPreprocessComputed)
        || sourceReload;

    /*
     * rawSourceWorkRuns(): M_RAW/M_CSHARP, a pending high-detail demosaic, the
     * colour highlight-recovery condition of a modified RGB source, and a
     * reload. A rebuild request is absent for the same reason as in
     * `preprocess`. The caller's extra DEMOSAIC augmentation for a white
     * balance on the Coloropp path is a mask fact and is deliberately not
     * copied: it appears as a legacy-only run in the shadow.
     */
    const bool demosaic = blockChanged(inputs, "raw.demosaic")
        || (current.highDetailNeeded && !current.highDetailRawComputed)
        || hlr || sourceReload;

    /*
     * baseRebuildRuns(): the raw.base families that actually rebuild, plus the
     * forward edges that regenerate the planes the base reads. The raw.base
     * block fingerprint is not used: it merges spot and retinex with the
     * rebuilding families. A preprocess or a demosaic regenerates the planes
     * the base consumes (the forward-edge rule of stage_dirty_blocks.h,
     * close_dirty_blocks).
     */
    const bool base = preprocess || demosaic
        || paramFamilyChanged(inputs, &ProcParams::wb)
        || paramFamilyChanged(inputs, &ProcParams::toneCurve)
        || paramFamilyChanged(inputs, &ProcParams::filmNegative)
        || paramFamilyChanged(inputs, &ProcParams::retinex)
        || paramFamilyChanged(inputs, &ProcParams::cg)
        || baseIcmChanged(inputs)
        || rebuild;

    /*
     * A spot delta does not rebuild the base: the legacy M_SPOT bit only
     * enters the spot gates inside raw.base and the buffer restore. The
     * request fact `spotPending` already carries the enabled/entries test.
     */
    const bool spotsConfigured = inputs.params != nullptr
        && inputs.params->spot.enabled && !inputs.params->spot.entries.empty();
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
     * when its own parameters move or when a step that regenerates those
     * planes runs. A white-balance-only M_INIT does not regenerate the planes,
     * so the legacy gate over-approximates there.
     */
    if (retinexEnabled
        && (paramFamilyChanged(inputs, &ProcParams::retinex) || preprocess
            || demosaic)) {
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
     * needed when the buffer path ran or a buffer is still live. A disabled or
     * empty spot tool deletes the buffer, hence the `spotsConfigured` guard.
     */
    if ((current.spotBufferLive && spotsConfigured)
        || (spotApply && !base)) {
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
     * run. The crop never preprocesses, demosaics or prepares retinex.
     */
    if (inputs.currentState == nullptr || inputs.currentFingerprints == nullptr) {
        setAll(order, {WorkItem::sourceSelect, WorkItem::rawBaseRebuild,
                       WorkItem::spotApply, WorkItem::spotRestore,
                       WorkItem::analysisRun, WorkItem::denoiseRun});
        return order;
    }

    assert(inputs.requestKind == inputs.currentState->requestKind
           && "WorkInputs::requestKind must match the current render state");

    const RenderStateKey& current = *inputs.currentState;
    const bool history = usableHistory(inputs);
    const RenderStateKey& previous =
        history ? *inputs.previousState : current;

    const ProcParams* params = inputs.params;
    const bool denoiseEnabled = params == nullptr || params->dirpyrDenoise.enabled;
    const bool spotsConfigured = params != nullptr
        && params->spot.enabled && !params->spot.entries.empty();
    const bool spotChanged = spotsConfigured
        && paramFamilyChanged(inputs, &ProcParams::spot);

    /*
     * The crop folds its reload bits (M_INIT | M_LINDENOISE | M_HDR) into
     * RequestKind::FullCrop before it builds the key (dcrop.cc:679-686). A
     * source reload and a high-detail decision change the pixels the ROI is
     * cut from, so they reload it as well. No history means the first update,
     * which the legacy path runs with ALL.
     */
    const bool highDetailMoved = history
        && previous.highDetailNeeded != current.highDetailNeeded;
    const bool reload = !history
        || request_rebuilds_path(inputs.requestKind)
        || (history && sourceReloaded(previous, current)) || highDetailMoved;

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

    if ((current.spotBufferLive && spotsConfigured)
        || (spotApply && !reload)) {
        order.set(WorkItem::spotRestore);
    }

    return order;
}

} // namespace rtengine::stage
