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

#include "stage_device.h"

#include <memory>
#include <string>
#include <vector>

namespace rtengine::stage
{

/*
 * OpenCL backend for the stage runtime.
 *
 * The backend owns its own ICD loader state and context. It deliberately does
 * not share the classic engine's context: the graph runtime must be able to
 * run a node on a device without the classic pipeline being enabled, and the
 * two backends must not interfere through global state.
 */

struct StageOpenCLDeviceInfo {
    std::string platform;
    std::string device;
    std::string driver;
    std::string version;
    bool cpu = false;
    unsigned long long global_memory = 0;
    /* Platforms the ICD health probe skipped, empty when every platform
     * answered. An ICD that crashes the query is not fatal: its platforms are
     * skipped with this diagnostic and the node falls back to the host. */
    std::string note;
};

/* True when a usable device is present. A CPU device requires
 * RT_OPENCL_ALLOW_CPU=1, because it is not an accelerated path. */
bool opencl_stage_available(std::string* reason = nullptr);

/* Descriptor of the selected device; false when no device is available. */
bool opencl_stage_device_info(StageOpenCLDeviceInfo* info);

/* Nodes with an OpenCL implementation in this build. */
std::vector<std::string> opencl_stage_nodes();
bool has_opencl_stage_node(const std::string& name);

/* Device factory used by stage_device.cc. */
std::shared_ptr<StageDevice> make_opencl_stage_device();

} // namespace rtengine::stage
