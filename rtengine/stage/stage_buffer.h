#pragma once

#include "stage_abi.h"

namespace rtengine
{
class Imagefloat;
class LabImage;
class Image8;
}

namespace rtengine::stage
{

/* Non-owning descriptors over the engine's existing buffers. */
RtStageBuffer view(Imagefloat& image);
RtStageBuffer view(const Imagefloat& image);
RtStageBuffer view(LabImage& image);
RtStageBuffer view(const LabImage& image);
RtStageBuffer view(Image8& image);

bool valid(const RtStageBuffer& buffer);
size_t bytes_per_pixel(RtStageFormat format);
size_t row_bytes(const RtStageBuffer& buffer);

} // namespace rtengine::stage
