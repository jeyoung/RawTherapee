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

#include "stage_param_hash.h"

#include <string>
#include <vector>

namespace rtengine::procparams
{
class ProcParams;
}

namespace rtengine::stage
{

/*
 * Parameter provenance of one export stage descriptor.
 *
 * `fields` names the ProcParams members the block reads, not individual
 * members of those sub-structs. Whole sub-structs are hashed, so a fingerprint
 * is a conservative over-approximation: a field that the block ignores still
 * changes it. That is deliberate, because a stale cache entry is a correctness
 * failure while a spurious cache miss is only a performance cost.
 *
 * The table is ordered like the export pipeline and is part of the review
 * surface for the parameter map. Ids are the canonical descriptor ids from
 * rtengine/stage/stage_pipeline.cc, and every field name is a ProcParams
 * member.
 */
struct ExportParamMapEntry {
    std::string id;
    std::vector<std::string> fields;
};

/*
 * Stable descriptor-to-fields table for the twenty export descriptors.
 *
 * The returned vector is built once and is immutable afterwards, so the
 * address is valid for the lifetime of the process.
 */
const std::vector<ExportParamMapEntry>& exportParamMap();

/*
 * Whether `id` is an export descriptor with a parameter fingerprint.
 *
 * Only the canonical export ids qualify. Preview, crop, thumbnail and
 * monitor/viewport ids are rejected, as is any unknown string.
 */
bool has_export_fingerprint(const std::string& id);

/*
 * Parameter fingerprint of one export stage.
 *
 * The hasher is labelled with `id` before any field is added, so two
 * descriptors never share a fingerprint stream even when they list the same
 * member. Identical parameters therefore give identical fingerprints, and a
 * change in a listed member gives a different one.
 *
 * Precondition: has_export_fingerprint(id). A violated precondition trips an
 * assertion in debug builds and returns an all-zero fingerprint otherwise.
 *
 * A fingerprint is persisted in stage cache keys, so the field list and its
 * order are part of the fingerprint format. Changing either invalidates every
 * stored stage fingerprint; do it only with the rest of the format change.
 */
ParamFingerprint export_fingerprint(const std::string& id,
                                    const procparams::ProcParams& params);

} // namespace rtengine::stage
