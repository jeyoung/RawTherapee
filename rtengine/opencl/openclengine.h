/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2026 The RawTherapee Team
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

#include <string>
#include <vector>

namespace rtengine
{

class Imagefloat;
class LabImage;

namespace opencl
{

/** @brief A device as shown in the preferences and diagnostics. */
struct DeviceInfo {
    std::string name;
    std::string vendor;
    std::string version;
    std::string driver;
    unsigned long long globalMemory = 0;
    unsigned long long maxAllocation = 0;
    unsigned long long localMemory = 0;
    unsigned long long maxWorkGroup = 0;
    bool cpu = false;
};

/** @brief Enumerate devices and prepare the backend. Safe to call twice. */
bool init();

/** @brief Release all OpenCL resources. */
void shutdown();

/** @brief Re-enumerate devices, e.g. after the user changes the selection. */
void reconfigure();

/** @brief True when the build has OpenCL support and at least one device. */
bool available();

/** @brief True when the user enabled acceleration and a device is available. */
bool enabled();

/** @brief Devices discovered during the last init/reconfigure. */
std::vector<DeviceInfo> devices();

/** @brief Index of the selected device, or -1 when none is available. */
int selectedDevice();

/** @brief Run a small end-to-end test, including kernel compilation. */
bool selfTest(std::string& report);

/** @brief Resize an RGB image with Lanczos, matching ImProcFunctions::Lanczos. */
bool resizeLanczos(const Imagefloat* src, Imagefloat* dst, float scale);

/** @brief Resize a Lab image with Lanczos, matching ImProcFunctions::Lanczos. */
bool resizeLanczos(const LabImage* src, LabImage* dst, float scale);

/** @brief Rotation and scale transform with bicubic or bilinear sampling. */
bool transformRotateScale(const Imagefloat* src, Imagefloat* dst,
                          int cx, int cy, int sx, int sy, int oW, int oH,
                          double hscale, double vscale,
                          double cost, double sint, bool highQuality,
                          bool useLog);

/** @brief Allow GPU stages in the current worker thread. */
void setStageAllowed(bool allowed);

/** @brief Whether GPU stages are allowed in the current worker thread. */
bool stageAllowed();

/** @brief Capture-sharpening deconvolution on a raw luminance plane. */
bool captureSharpening(float** luminance, const float* const* oldLuminance,
                       const float* const* blend, int width, int height,
                       float sigma, float sigmaCornerOffset, int iterations,
                       bool checkIterStop);

/** @brief Short human readable state, also used by --gpu-info. */
std::string statusText();

} // namespace opencl

} // namespace rtengine
