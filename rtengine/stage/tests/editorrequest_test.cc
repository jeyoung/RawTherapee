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
     * planning information must have the same identity. The legacy gate mask
     * is deliberately excluded.
     */
    const EditorRequest autoexp = from_legacy_mask(M_AUTOEXP);
    const EditorRequest color = from_legacy_mask(M_COLOR);
    check(autoexp.identity() == color.identity(),
          "identity ignores the legacy mask");
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
}

} // namespace

int main()
{
    checkEveryBit();
    checkControlAndImperative();
    checkCombinedBits();
    checkViewportMovedContext();
    checkIdentity();
    checkContractViolations();

    if (failures == 0) {
        std::puts("editorrequest-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
