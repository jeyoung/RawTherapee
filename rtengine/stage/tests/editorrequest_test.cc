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
 * Specification test for the typed editor request and its legacy adapter.
 *
 * The expected rows below are the contract. Every `M_*` bit the coordinator
 * consumes must map to exactly one request kind and set of facts, M_VOID must
 * suppress the render, M_MINUPDATE alone must yield RepaintOnly, and an
 * unmapped bit or a combined M_MINUPDATE must fail fast instead of silently
 * degrading to ParamsOnly.
 */

#include "rtengine/editorrequest.h"

#include "rtengine/refreshmap.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

using rtengine::EditorRequest;
using rtengine::LegacyMaskContext;
using rtengine::coalesce;
using rtengine::from_legacy_mask;
using rtengine::planningAgreesWithLegacyMask;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void checkBit(int mask, const char* name, EditorRequest::RequestKind kind,
              bool highDetail, bool fullFrame, bool spot, bool controlOnly)
{
    const EditorRequest request = from_legacy_mask(mask);
    const std::string label = name;

    check(request.kind == kind, label + ": kind");
    check(request.present, label + ": present");
    check(request.controlOnly == controlOnly, label + ": controlOnly");
    check(request.highDetailIntent == highDetail,
          label + ": highDetailIntent");
    check(request.fullFrameIntent == fullFrame, label + ": fullFrameIntent");
    check(request.spotPending == spot, label + ": spotPending");
    check(request.legacyGateMask == mask, label + ": legacyGateMask");
    check(request.derivesFromParameters(),
          label + ": derives from parameters");
    check(request.dirty.blocks.empty(), label + ": no producer dirty set");

    /*
     * The mask-shaped facts. For a single bit, only M_MONITOR admits the
     * full-frame path, only M_WB requests the Color/Coloropp DEMOSAIC, only a
     * bare M_CROP or M_MINUPDATE suppresses monitor.output, and only
     * M_MINUPDATE is an imperative repaint.
     */
    check(request.fullFrameAdmission == (mask == M_MONITOR),
          label + ": fullFrameAdmission");
    check(request.colorHlrWbRequest == (mask == M_WB),
          label + ": colorHlrWbRequest");
    check(request.monitorOutputRequest == (mask != M_CROP && mask != M_MINUPDATE),
          label + ": monitorOutputRequest");
    check(request.imperativeRepaint == (mask == M_MINUPDATE),
          label + ": imperativeRepaint");
}

struct BitExpectation {
    int bit;
    const char* name;
    EditorRequest::RequestKind kind;
    bool highDetail;
    bool fullFrame;
    bool spot;
    bool controlOnly;
};

const std::vector<BitExpectation>& expectations()
{
    static const std::vector<BitExpectation> rows = {
        {M_PREPROC, "M_PREPROC", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RAW, "M_RAW", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_CSHARP, "M_CSHARP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_WB, "M_WB", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_SPOT, "M_SPOT", EditorRequest::RequestKind::ParamsOnly,
         false, false, true, false},
        {M_INIT, "M_INIT", EditorRequest::RequestKind::FullPreview,
         false, true, false, false},
        {M_LINDENOISE, "M_LINDENOISE",
         EditorRequest::RequestKind::FullPreview, false, true, false, false},
        {M_HDR, "M_HDR", EditorRequest::RequestKind::FullPreview,
         false, true, false, false},
        {M_TRANSFORM, "M_TRANSFORM", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RETINEX, "M_RETINEX", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_BLURMAP, "M_BLURMAP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_AUTOEXP, "M_AUTOEXP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_RGBCURVE, "M_RGBCURVE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_LUMACURVE, "M_LUMACURVE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_LUMINANCE, "M_LUMINANCE", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_COLOR, "M_COLOR", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_CROP, "M_CROP", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_MONITOR, "M_MONITOR", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, false},
        {M_VOID, "M_VOID", EditorRequest::RequestKind::ParamsOnly,
         false, false, false, true},
        {M_MINUPDATE, "M_MINUPDATE", EditorRequest::RequestKind::RepaintOnly,
         false, false, false, false},
        {M_HIGHQUAL, "M_HIGHQUAL", EditorRequest::RequestKind::HighQuality,
         true, false, false, false}
    };
    return rows;
}

void checkEveryBit()
{
    int covered = 0;
    for (const BitExpectation& row : expectations()) {
        checkBit(row.bit, row.name, row.kind, row.highDetail, row.fullFrame,
                 row.spot, row.controlOnly);
        covered |= row.bit;
    }

    /* Every low bit the engine defines, and nothing above them. */
    const int expected = (1 << 21) - 1;
    check(covered == expected,
          "the table covers every consumed M_* bit exactly once");
}

void checkControlAndImperative()
{
    const EditorRequest control = from_legacy_mask(M_VOID);
    check(control.controlOnly, "M_VOID is control-only");
    check(control.present, "M_VOID is still a request");
    check(from_legacy_mask(0).present == false, "zero mask is not a request");
    check(from_legacy_mask(0).controlOnly, "zero mask is control-only");

    const EditorRequest repaint = from_legacy_mask(M_MINUPDATE);
    check(repaint.kind == EditorRequest::RequestKind::RepaintOnly,
          "M_MINUPDATE alone is RepaintOnly");
    check(!repaint.controlOnly, "M_MINUPDATE is imperative");

    const EditorRequest twice = coalesce(repaint, repaint);
    check(twice.kind == EditorRequest::RequestKind::RepaintOnly,
          "two repaints coalesce to RepaintOnly");
    check(twice.legacyGateMask == M_MINUPDATE,
          "two repaints keep the bare MINUPDATE mask");

    /*
     * A control-only request combined with a repaint is the old
     * `M_MINUPDATE | M_VOID`: no longer exactly M_MINUPDATE, so ParamsOnly.
     */
    const EditorRequest poisoned = coalesce(repaint, control);
    check(poisoned.kind == EditorRequest::RequestKind::ParamsOnly,
          "control + repaint is ParamsOnly");
    check(poisoned.legacyGateMask == (M_MINUPDATE | M_VOID),
          "control + repaint unions the gate mask");

    const EditorRequest withParams =
        coalesce(repaint, from_legacy_mask(M_CROP));
    check(withParams.kind == EditorRequest::RequestKind::ParamsOnly,
          "repaint + parameter change is ParamsOnly");
}

void checkCombinedBits()
{
    const EditorRequest reload =
        from_legacy_mask(M_INIT | M_HDR | M_CROP);
    check(reload.kind == EditorRequest::RequestKind::FullPreview,
          "reload + crop is FullPreview");
    check(reload.fullFrameIntent, "reload sets fullFrameIntent");
    check(!reload.spotPending, "reload + crop does not request spot work");
    check(!reload.controlOnly, "reload is not control-only");

    const EditorRequest accurate =
        from_legacy_mask(M_HIGHQUAL | M_INIT | M_SPOT);
    check(accurate.kind == EditorRequest::RequestKind::HighQuality,
          "M_HIGHQUAL wins over reload bits");
    check(accurate.highDetailIntent, "M_HIGHQUAL sets highDetailIntent");
    check(accurate.fullFrameIntent,
          "M_HIGHQUAL with M_INIT still records full-frame intent");
    check(accurate.spotPending, "M_HIGHQUAL with M_SPOT records spot intent");

    const EditorRequest coalesced =
        coalesce(from_legacy_mask(M_INIT),
                 from_legacy_mask(M_HIGHQUAL | M_CROP));
    check(coalesced.kind == EditorRequest::RequestKind::HighQuality,
          "coalescing keeps the strongest kind");
    check(coalesced.fullFrameIntent,
          "coalescing unions the request facts");
    check(coalesced.legacyGateMask == (M_INIT | M_HIGHQUAL | M_CROP),
          "coalescing unions the gate mask");
}

/*
 * Every pair of bits that can legally be coalesced must produce exactly the
 * request a single mask of the union produces: same kind, same facts, same
 * gate mask and same identity. Pairs whose union contains a combined
 * M_MINUPDATE are contract violations and are checked separately.
 */
void checkEveryPair()
{
    for (const BitExpectation& left : expectations()) {
        for (const BitExpectation& right : expectations()) {
            const int unionMask = left.bit | right.bit;

            if ((unionMask & M_MINUPDATE) != 0
                    && unionMask != M_MINUPDATE) {
                continue;
            }

            const EditorRequest merged =
                coalesce(from_legacy_mask(left.bit),
                         from_legacy_mask(right.bit));
            const EditorRequest single = from_legacy_mask(unionMask);
            const std::string label =
                std::string("pair ") + left.name + "+" + right.name;

            check(merged.present, label + ": present");
            check(merged.kind == single.kind, label + ": kind");
            check(merged.controlOnly == single.controlOnly,
                  label + ": controlOnly");
            check(merged.spotPending == single.spotPending,
                  label + ": spotPending");
            check(merged.highDetailIntent == single.highDetailIntent,
                  label + ": highDetailIntent");
            check(merged.fullFrameIntent == single.fullFrameIntent,
                  label + ": fullFrameIntent");
            check(merged.fullFrameAdmission == single.fullFrameAdmission,
                  label + ": fullFrameAdmission");
            check(merged.colorHlrWbRequest == single.colorHlrWbRequest,
                  label + ": colorHlrWbRequest");
            check(merged.monitorOutputRequest == single.monitorOutputRequest,
                  label + ": monitorOutputRequest");
            check(merged.imperativeRepaint == single.imperativeRepaint,
                  label + ": imperativeRepaint");
            check(merged.legacyGateMask == unionMask,
                  label + ": gate mask");
            check(merged.identity() == single.identity(),
                  label + ": identity");
        }
    }
}

/*
 * Facts that are monotone in the mask are unioned; facts that interpret the
 * mask as a whole are recomputed from the union.
 */
void checkFactUnion()
{
    LegacyMaskContext moved;
    moved.viewportMoved = true;

    const EditorRequest spotAndReload =
        coalesce(from_legacy_mask(M_SPOT), from_legacy_mask(M_INIT));
    check(spotAndReload.spotPending && spotAndReload.fullFrameIntent,
          "coalescing unions spot and full-frame intent");

    const EditorRequest movedAndHigh =
        coalesce(from_legacy_mask(M_CROP, moved),
                 from_legacy_mask(M_HIGHQUAL));
    check(movedAndHigh.viewportMoved && movedAndHigh.highDetailIntent,
          "coalescing unions viewportMoved and high-detail intent");
    check(movedAndHigh.kind == EditorRequest::RequestKind::HighQuality,
          "high detail keeps its precedence over a viewport move");

    const EditorRequest controlAndCrop =
        coalesce(from_legacy_mask(M_VOID), from_legacy_mask(M_CROP));
    check(!controlAndCrop.controlOnly,
          "coalescing ANDs controlOnly over a rendering request");

    /*
     * Both inputs suppress monitor.output. Their union,
     * `M_CROP | M_MINUPDATE`, is neither exactly `M_CROP` nor exactly
     * `M_MINUPDATE`, so the legacy predicate renders it. This proves the
     * fact is recomputed from the union rather than ORed.
     */
    const EditorRequest cropAndRepaint =
        coalesce(from_legacy_mask(M_CROP), from_legacy_mask(M_MINUPDATE));
    check(!from_legacy_mask(M_CROP).monitorOutputRequest,
          "a bare crop suppresses monitor.output");
    check(!from_legacy_mask(M_MINUPDATE).monitorOutputRequest,
          "a bare repaint suppresses monitor.output");
    check(cropAndRepaint.monitorOutputRequest,
          "the union of two monitor-suppressing masks renders monitor.output");

    /*
     * Neither input sets every ALL bit, but their union does, so the merged
     * request admits the full-frame path. Admission is not a monotone union.
     */
    const int left = M_PREPROC | M_RAW;
    const int right = ALL & ~left;
    check(!from_legacy_mask(left).fullFrameAdmission,
          "half of ALL does not admit the full frame");
    check(!from_legacy_mask(right).fullFrameAdmission,
          "the other half of ALL does not admit the full frame");
    check(coalesce(from_legacy_mask(left), from_legacy_mask(right))
              .fullFrameAdmission,
          "the union of two halves of ALL admits the full frame");
}

void checkEmptyAccumulator()
{
    const EditorRequest empty;
    check(!empty.present, "an empty accumulator is not present");
    check(empty.controlOnly, "an empty accumulator is control-only");

    const EditorRequest request = from_legacy_mask(M_HIGHQUAL | M_CROP);

    const EditorRequest withEmptyFirst = coalesce(empty, request);
    check(withEmptyFirst.identity() == request.identity(),
          "an empty first accumulator returns the second request");
    check(withEmptyFirst.legacyGateMask == request.legacyGateMask,
          "an empty first accumulator keeps the second mask");

    const EditorRequest withEmptySecond = coalesce(request, empty);
    check(withEmptySecond.identity() == request.identity(),
          "an empty second accumulator returns the first request");
    check(withEmptySecond.legacyGateMask == request.legacyGateMask,
          "an empty second accumulator keeps the first mask");

    check(!coalesce(empty, empty).present,
          "coalescing two empty accumulators stays empty");
}

/*
 * The coordinator patches the typed kind and facts after it widens a legacy
 * mask at four sites. These are the exact shapes it produces; the debug
 * cross-check in the coordinator asserts the same relation.
 */
void checkCoordinatorPatches()
{
    {
        EditorRequest requeued = from_legacy_mask(M_MINUPDATE);
        check(requeued.imperativeRepaint,
              "a bare repaint is imperative before the requeue");
        requeued.setLegacyGateMask(M_MINUPDATE | M_MONITOR);
        requeued.viewportMoved = true;
        requeued.kind = EditorRequest::RequestKind::ParamsOnly;
        check(!requeued.imperativeRepaint,
              "widening the requeue clears the imperative repaint");
        check(planningAgreesWithLegacyMask(requeued),
              "the panning requeue patch agrees with its widened mask");
    }
    {
        EditorRequest refreshed = from_legacy_mask(M_CROP);
        refreshed.setLegacyGateMask(M_CROP | M_INIT | M_HDR | M_MONITOR);
        refreshed.fullFrameIntent = true;
        refreshed.kind = EditorRequest::RequestKind::FullPreview;
        check(planningAgreesWithLegacyMask(refreshed),
              "the base-refresh patch agrees with its widened mask");
    }
    {
        EditorRequest absorbed = from_legacy_mask(M_MINUPDATE);
        absorbed.setLegacyGateMask(
            M_MINUPDATE | M_MONITOR | M_RGBCURVE | M_LUMACURVE
            | M_LUMINANCE | M_COLOR);
        absorbed.kind = EditorRequest::RequestKind::ParamsOnly;
        check(planningAgreesWithLegacyMask(absorbed),
              "the deferred-absorb patch agrees with its widened mask");
    }
    {
        EditorRequest color = from_legacy_mask(M_WB);
        color.setLegacyGateMask(M_WB | DEMOSAIC);
        color.fullFrameIntent = true;
        color.spotPending = true;
        color.kind = EditorRequest::RequestKind::FullPreview;
        check(planningAgreesWithLegacyMask(color),
              "the Color/Coloropp patch agrees with its widened mask");
    }

    /* The cross-check is not vacuous: a wrong kind must be rejected. */
    EditorRequest wrong = from_legacy_mask(M_WB);
    wrong.kind = EditorRequest::RequestKind::FullPreview;
    check(!planningAgreesWithLegacyMask(wrong),
          "the cross-check rejects a kind the mask does not imply");
}

void checkViewportMovedContext()
{
    LegacyMaskContext context;
    context.viewportMoved = true;

    const EditorRequest moved = from_legacy_mask(M_CROP, context);
    check(moved.viewportMoved, "the adapter carries viewportMoved");
    check(!from_legacy_mask(M_CROP).viewportMoved,
          "viewportMoved defaults to false");
}

void checkIdentity()
{
    /*
     * The identity is a planning identity: two different masks that carry no
     * planning information must have the same identity. The raw legacy mask
     * is deliberately excluded; every mask-shaped planning fact is included.
     */
    const EditorRequest autoexp = from_legacy_mask(M_AUTOEXP);
    const EditorRequest color = from_legacy_mask(M_COLOR);
    check(autoexp.identity() == color.identity(),
          "identity ignores the raw legacy mask");
    check(autoexp.identity()
              == coalesce(autoexp, color).identity(),
          "coalescing planning-equivalent requests keeps the identity");

    check(autoexp.identity() != from_legacy_mask(M_SPOT).identity(),
          "spot intent changes the identity");
    check(autoexp.identity() != from_legacy_mask(M_INIT).identity(),
          "full-frame intent changes the identity");
    check(autoexp.identity() != from_legacy_mask(M_HIGHQUAL).identity(),
          "high-detail intent changes the identity");
    check(autoexp.identity() != from_legacy_mask(M_MINUPDATE).identity(),
          "kind changes the identity");

    LegacyMaskContext moved;
    moved.viewportMoved = true;
    check(autoexp.identity() != from_legacy_mask(M_AUTOEXP, moved).identity(),
          "viewportMoved changes the identity");

    /*
     * Mask-shaped facts. M_WB and M_AUTOEXP plan differently (the Color/Coloropp
     * DEMOSAIC path), and M_MONITOR and M_COLOR plan differently (full-frame
     * admission), so neither pair may share an identity.
     */
    check(from_legacy_mask(M_WB).identity()
              != from_legacy_mask(M_AUTOEXP).identity(),
          "colorHlrWbRequest changes the identity");
    check(from_legacy_mask(M_MONITOR).identity()
              != from_legacy_mask(M_COLOR).identity(),
          "fullFrameAdmission changes the identity");
    check(from_legacy_mask(M_COLOR).identity()
              != from_legacy_mask(M_CROP).identity(),
          "monitorOutputRequest changes the identity");

    /* The imperative-repaint fact is a planning field of its own. */
    EditorRequest imperative = from_legacy_mask(M_MINUPDATE);
    EditorRequest notImperative = imperative;
    notImperative.imperativeRepaint = false;
    check(imperative.identity() != notImperative.identity(),
          "imperativeRepaint changes the identity");
}

/*
 * The adapter must abort on a contract violation in every build, not only
 * where NDEBUG leaves the assertion in. Run the call in a child process.
 */
void expectAbort(int mask, const char* label)
{
    const pid_t pid = fork();

    if (pid == 0) {
        from_legacy_mask(mask);
        _exit(0);
    }

    if (pid < 0) {
        check(false, std::string(label) + ": fork failed");
        return;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        check(false, std::string(label) + ": waitpid failed");
        return;
    }

    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          std::string(label) + ": expected an abort");
}

void checkContractViolations()
{
    expectAbort(1 << 21, "unknown bit");
    expectAbort(M_MINUPDATE | M_CROP, "combined M_MINUPDATE");
    expectAbort(M_MINUPDATE | M_VOID, "M_MINUPDATE with M_VOID");
    expectAbort(M_MINUPDATE | M_HIGHQUAL, "M_MINUPDATE with M_HIGHQUAL");
}

} // namespace

int main()
{
    checkEveryBit();
    checkControlAndImperative();
    checkCombinedBits();
    checkEveryPair();
    checkFactUnion();
    checkEmptyAccumulator();
    checkCoordinatorPatches();
    checkViewportMovedContext();
    checkIdentity();
    checkContractViolations();

    if (failures == 0) {
        std::puts("editorrequest-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
