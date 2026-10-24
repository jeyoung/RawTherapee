#include "stage_buffer.h"

#include "image8.h"
#include "imagefloat.h"
#include "labimage.h"

#include <cassert>

namespace rtengine::stage
{

namespace
{

RtStageBuffer make(void* data, int width, int height, size_t row_pitch,
                   size_t plane_pitch, uint32_t planes, RtStageFormat format,
                   uint32_t flags)
{
    RtStageBuffer buffer{};
    buffer.size = sizeof(buffer);
    buffer.data = data;
    buffer.width = width;
    buffer.height = height;
    buffer.row_pitch = row_pitch;
    buffer.plane_pitch = plane_pitch;
    buffer.planes = planes;
    buffer.format = format;
    buffer.flags = flags;
    return buffer;
}

} // namespace

RtStageBuffer view(Imagefloat& image)
{
    const uint32_t flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return make(image.r(0), image.getWidth(), image.getHeight(),
                image.getRowStride(), image.getPlaneStride(), 3,
                RT_STAGE_F32, flags);
}

RtStageBuffer view(const Imagefloat& image)
{
    return make(const_cast<float*>(image.r(0)), image.getWidth(), image.getHeight(),
                image.getRowStride(), image.getPlaneStride(), 3,
                RT_STAGE_F32, RT_STAGE_BUFFER_READ);
}

RtStageBuffer view(LabImage& image)
{
    const size_t row_pitch = static_cast<size_t>(image.W) * sizeof(float);
    const size_t plane_pitch = row_pitch * static_cast<size_t>(image.H);
    return make(image.L[0], image.W, image.H, row_pitch, plane_pitch, 3,
                RT_STAGE_F32, RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE);
}

RtStageBuffer view(const LabImage& image)
{
    const size_t row_pitch = static_cast<size_t>(image.W) * sizeof(float);
    const size_t plane_pitch = row_pitch * static_cast<size_t>(image.H);
    return make(const_cast<float*>(image.L[0]), image.W, image.H, row_pitch,
                plane_pitch, 3, RT_STAGE_F32, RT_STAGE_BUFFER_READ);
}

RtStageBuffer view(Image8& image)
{
    const size_t row_pitch = static_cast<size_t>(image.getWidth()) * 3;
    const size_t plane_pitch = row_pitch * static_cast<size_t>(image.getHeight());
    return make(image.data, image.getWidth(), image.getHeight(), row_pitch,
                plane_pitch, 1, RT_STAGE_U8,
                RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE);
}

bool valid(const RtStageBuffer& buffer)
{
    return buffer.size == sizeof(RtStageBuffer) && buffer.data != nullptr &&
           buffer.width > 0 && buffer.height > 0 && buffer.planes > 0 &&
           buffer.row_pitch >= static_cast<size_t>(buffer.width) *
                                  bytes_per_pixel(buffer.format);
}

size_t bytes_per_pixel(RtStageFormat format)
{
    switch (format) {
    case RT_STAGE_F32:
        return sizeof(float);
    case RT_STAGE_U16:
        return sizeof(uint16_t);
    case RT_STAGE_U8:
        return sizeof(uint8_t);
    }
    assert(false);
    return 0;
}

size_t row_bytes(const RtStageBuffer& buffer)
{
    return static_cast<size_t>(buffer.width) * bytes_per_pixel(buffer.format) *
           (buffer.format == RT_STAGE_U8 ? 3u : 1u);
}

} // namespace rtengine::stage
