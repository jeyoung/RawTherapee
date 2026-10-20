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

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif

#include <CL/cl.h>

#include <glibmm/module.h>

#include <string>

namespace rtengine
{

namespace opencl
{

/**
 * @brief The subset of the OpenCL entry points used by RawTherapee.
 *
 * The ICD loader is opened with Glib::Module so that the application has no
 * link time dependency on OpenCL and still starts on systems without it.
 */
struct Api {
    cl_int (CL_API_CALL *GetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*) = nullptr;
    cl_int (CL_API_CALL *GetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void*, size_t*) = nullptr;
    cl_int (CL_API_CALL *GetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*) = nullptr;
    cl_int (CL_API_CALL *GetDeviceInfo)(cl_device_id, cl_device_info, size_t, void*, size_t*) = nullptr;

    cl_context (CL_API_CALL *CreateContext)(const cl_context_properties*, cl_uint, const cl_device_id*,
                                void (CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseContext)(cl_context) = nullptr;
    cl_command_queue (CL_API_CALL *CreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseCommandQueue)(cl_command_queue) = nullptr;
    cl_mem (CL_API_CALL *CreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseMemObject)(cl_mem) = nullptr;
    cl_int (CL_API_CALL *EnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                 const void*, cl_uint, const cl_event*, cl_event*) = nullptr;
    cl_int (CL_API_CALL *EnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                void*, cl_uint, const cl_event*, cl_event*) = nullptr;
    cl_int (CL_API_CALL *EnqueueWriteBufferRect)(cl_command_queue, cl_mem, cl_bool,
                                     const size_t*, const size_t*, const size_t*,
                                     size_t, size_t, size_t, size_t,
                                     const void*, cl_uint, const cl_event*,
                                     cl_event*) = nullptr;
    cl_int (CL_API_CALL *EnqueueReadBufferRect)(cl_command_queue, cl_mem, cl_bool,
                                    const size_t*, const size_t*, const size_t*,
                                    size_t, size_t, size_t, size_t,
                                    void*, cl_uint, const cl_event*,
                                    cl_event*) = nullptr;
    cl_int (CL_API_CALL *Finish)(cl_command_queue) = nullptr;

    cl_program (CL_API_CALL *CreateProgramWithSource)(cl_context, cl_uint, const char**,
                                          const size_t*, cl_int*) = nullptr;
    cl_program (CL_API_CALL *CreateProgramWithBinary)(cl_context, cl_uint, const cl_device_id*,
                                          const size_t*, const unsigned char**,
                                          cl_int*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *GetProgramInfo)(cl_program, cl_program_info, size_t, void*, size_t*) = nullptr;
    cl_int (CL_API_CALL *BuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*,
                           void (CL_CALLBACK*)(cl_program, void*), void*) = nullptr;
    cl_int (CL_API_CALL *GetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info,
                                  size_t, void*, size_t*) = nullptr;
    cl_kernel (CL_API_CALL *CreateKernel)(cl_program, const char*, cl_int*) = nullptr;
    cl_int (CL_API_CALL *ReleaseKernel)(cl_kernel) = nullptr;
    cl_int (CL_API_CALL *ReleaseProgram)(cl_program) = nullptr;
    cl_int (CL_API_CALL *SetKernelArg)(cl_kernel, cl_uint, size_t, const void*) = nullptr;
    cl_int (CL_API_CALL *EnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*,
                                   const size_t*, const size_t*, cl_uint, const cl_event*,
                                   cl_event*) = nullptr;
};

/** @brief Open the ICD loader and resolve the entry points above. */
bool loadApi(Glib::Module& module, Api& api, std::string& error);

} // namespace opencl

} // namespace rtengine
