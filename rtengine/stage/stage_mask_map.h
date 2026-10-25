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

#pragma once

#include "stage_pipeline.h"

#include <string>
#include <vector>

namespace rtengine::stage
{

/*
 * Equivalence map from the legacy `M_*` bitfield in rtengine/refreshmap.h to
 * the canonical stage descriptors in rtengine/stage/stage_pipeline.h.
 *
 * The legacy bitset mixes four different things, and the map keeps them apart:
 *
 * - Execution bits dirty one or more processing blocks because a parameter
 *   family the block reads has changed.
 * - Output-request bits do not name a parameter family. `M_CROP` names crop or
 *   viewport geometry, `M_MONITOR` names a display or output-profile transform.
 * - Request-kind bits do not dirty any block. `M_VOID` suppresses the preview
 *   update, `M_MINUPDATE` asks for a repaint without new image work and
 *   `M_HIGHQUAL` forces the accurate path.
 *
 * The map is a parallel description, not a replacement: refreshmap.h and
 * refreshmap.cc are untouched and keep driving the legacy pipelines. Its
 * purpose is to let the stage runtime be validated against the bitset before
 * the bitset is removed, and to record which blocks the bitset cannot express.
 *
 * A returned block set is direct dirtiness, not a transitive closure. A
 * consumer that needs the complete recompute set must close it over the edges
 * of buildPipelineSpec(path). Returned ids are in canonical pipeline order and
 * are always members of that path's specification.
 *
 * The map covers StagePath::Export, StagePath::Preview and StagePath::Crop.
 * StagePath::Thumbnail has its own quick path and no bitset equivalence yet.
 */
enum class MaskBitKind {
    /* Dirties the listed blocks from a parameter family. */
    Execution,
    /* Names an output, view or geometry request rather than a block input. */
    OutputRequest,
    /* Controls how much work is done; never dirties a block. */
    RequestKind
};

const char* maskBitKindName(MaskBitKind kind);

/*
 * One row of the map: the bit, its symbolic name, its kind and the canonical
 * descriptor ids it dirties on each supported path.
 *
 * The three per-path vectors exist because two bits are genuinely path
 * specific. Every other row carries the same ids on the paths that contain
 * them: path filtering, not a second opinion, is what removes a block such as
 * `lab.sharpening` from the preview path.
 */
struct StageMaskMapEntry {
    int bit = 0;
    const char* name = "";
    MaskBitKind kind = MaskBitKind::Execution;
    std::vector<std::string> exportBlocks;
    std::vector<std::string> previewBlocks;
    std::vector<std::string> cropBlocks;
};

/*
 * The full table, in review order: execution bits in pipeline order, then
 * output requests, then request kinds. The returned vector is built once and
 * is immutable afterwards, so the address is valid for the process lifetime.
 */
const std::vector<StageMaskMapEntry>& stageMaskMap();

/* Whether `bit` appears in the table. Expected to be a single bit. */
bool isKnownMaskBit(int bit);

/*
 * Whether the row's block set deliberately differs between paths.
 *
 * Only `M_CROP` and `M_MONITOR` qualify: crop geometry and the output/display
 * transform mean different blocks on different paths. The consistency test
 * exempts these two and no others.
 */
bool maskBitIsPathSpecific(int bit);

/*
 * Blocks dirtied by `mask` on `path`.
 *
 * Precondition: `path` is Export, Preview or Crop, and every set bit in `mask`
 * belongs to a table row. A violated precondition trips an assertion in debug
 * builds; unknown bits are ignored otherwise, so a release build degrades to
 * under-invalidation rather than undefined behaviour.
 *
 * Composite constants from refreshmap.h (`ALL`, `COMPR`, `DEMOSAIC`, ...) work
 * unchanged because they are ordinary bit unions. Aliases that resolve to a
 * request kind, such as `RESIZE`, `EXIF` and `IPTC` (all `M_VOID`), return an
 * empty set by design.
 */
std::vector<std::string> blocks_for_mask(int mask, StagePath path);

/*
 * Best-effort inverse for migration diagnostics only.
 *
 * Returns the union of every non-request bit whose per-path block set is a
 * subset of `blocks` on at least one supported path. The inversion is
 * deliberately generous: several bits share a block, so a set that contains
 * `raw.base` also selects `M_WB` even when the original mask was `M_INIT`.
 * Request-kind bits are never inferred, because their empty block set is a
 * subset of every input; a caller that needs one must add it explicitly.
 *
 * Precondition: every id in `blocks` is a canonical descriptor of Export,
 * Preview or Crop. Unknown ids trip an assertion and are ignored.
 */
int mask_for_blocks(const std::vector<std::string>& blocks);

/*
 * Inputs that reach a block without passing through an `M_*` bit.
 *
 * The bitset is not the only source of work. A new source invalidates the
 * source block, the monitor profile and soft proof live in RenderStateKey, and
 * several export-only blocks are driven by events that map to `M_VOID` because
 * they must not repaint the preview. Recording them here lets the reachability
 * test prove that no block is silently unmapped.
 */
enum class StageInputKind {
    /* Image source identity or source generation. */
    Source,
    /* ProcParams members with no dedicated execution bit. */
    Parameters,
    /* RenderStateKey fields, which are not in ProcParams. */
    RenderState,
    /* A request that runs a whole path, such as an export or a thumbnail. */
    Request
};

const char* stageInputKindName(StageInputKind kind);

struct StageInputEntry {
    std::string block;
    StageInputKind kind = StageInputKind::Parameters;
    const char* reason = "";
};

/* Non-mask inputs for one path; empty for StagePath::Thumbnail. */
const std::vector<StageInputEntry>& nonMaskInputs(StagePath path);

} // namespace rtengine::stage
