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
#include "rtengine/opencl/openclengine.h"

namespace rtengine
{

namespace opencl
{

namespace
{

const std::string notBuilt = "OpenCL support is not compiled in";

} // namespace

bool init()
{
    return false;
}

void shutdown()
{
}

void reconfigure()
{
}

bool available()
{
    return false;
}

bool enabled()
{
    return false;
}

std::vector<DeviceInfo> devices()
{
    return {};
}

int selectedDevice()
{
    return -1;
}

bool selfTest(std::string& report)
{
    report = notBuilt;
    return false;
}

bool resizeLanczos(const Imagefloat*, Imagefloat*, float)
{
    return false;
}

bool resizeLanczos(const LabImage*, LabImage*, float)
{
    return false;
}

std::string statusText()
{
    return notBuilt;
}

} // namespace opencl

} // namespace rtengine
