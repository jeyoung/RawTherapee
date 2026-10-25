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

#include "stage_render_state.h"

namespace rtengine::stage
{

/*
 * The section label and the add() order are the persisted render-state
 * fingerprint format. Keep this function in step with the member order and
 * with the field-by-field stream in the test.
 */
ParamFingerprint RenderStateKey::fingerprint() const
{
    ParamHasher hasher;
    hasher.addSectionLabel("render-state");

    hasher.add(previewScale);
    hasher.add(previewWidth);
    hasher.add(previewHeight);
    hasher.add(fullWidth);
    hasher.add(fullHeight);

    hasher.add(cropX);
    hasher.add(cropY);
    hasher.add(cropWidth);
    hasher.add(cropHeight);
    hasher.add(cropSkip);
    hasher.add(borderRequested);
    hasher.add(borderLeft);
    hasher.add(borderTop);

    hasher.add(highDetailNeeded);
    hasher.add(highDetailPreprocessComputed);
    hasher.add(highDetailRawComputed);
    hasher.add(highDetailStateKnown);

    hasher.add(sourceGeneration);
    hasher.add(rgbSourceModified);

    hasher.addString(monitorProfile);
    hasher.add(monitorIntent);
    hasher.add(softProof);
    hasher.add(gamutCheck);

    hasher.add(sharpMask);
    hasher.add(fastPreviewEnabled);
    hasher.add(mainPreviewFullResolution);
    hasher.add(initialRoughPreview);

    hasher.add(static_cast<int32_t>(requestKind));
    hasher.add(previousDemoBase);

    /*
     * Request facts. Appending them changed the fingerprint format: the same
     * key hashed before and after the change yields different values, so a
     * stored key must not be compared with one written by an older build.
     */
    hasher.add(sourceRotateDegree);
    hasher.add(sourceSensorType);
    hasher.add(sourceFullWidth);
    hasher.add(sourceFullHeight);
    hasher.add(sourceFrameCount);
    hasher.add(dirPyrDenoiseExpComp);
    hasher.add(denoiseAnalysisValid);
    hasher.add(spotPending);
    hasher.add(spotBufferLive);

    return hasher.finish();
}

} // namespace rtengine::stage
