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
#include "rtengine/opencl/openclloader.h"

namespace rtengine
{

namespace opencl
{

namespace
{

template<typename T>
bool resolve(Glib::Module& module, const char* name, T& target, std::string& error)
{
    void* symbol = nullptr;

    if (!module.get_symbol(name, symbol) || symbol == nullptr) {
        error = name;
        return false;
    }

    target = reinterpret_cast<T>(symbol);
    return true;
}

} // namespace

bool loadApi(Glib::Module& module, Api& api, std::string& error)
{
    if (!resolve(module, "clGetPlatformIDs", api.GetPlatformIDs, error)
            || !resolve(module, "clGetPlatformInfo", api.GetPlatformInfo, error)
            || !resolve(module, "clGetDeviceIDs", api.GetDeviceIDs, error)
            || !resolve(module, "clGetDeviceInfo", api.GetDeviceInfo, error)
            || !resolve(module, "clCreateContext", api.CreateContext, error)
            || !resolve(module, "clReleaseContext", api.ReleaseContext, error)
            || !resolve(module, "clCreateCommandQueue", api.CreateCommandQueue, error)
            || !resolve(module, "clReleaseCommandQueue", api.ReleaseCommandQueue, error)
            || !resolve(module, "clCreateBuffer", api.CreateBuffer, error)
            || !resolve(module, "clReleaseMemObject", api.ReleaseMemObject, error)
            || !resolve(module, "clEnqueueWriteBuffer", api.EnqueueWriteBuffer, error)
            || !resolve(module, "clEnqueueReadBuffer", api.EnqueueReadBuffer, error)
            || !resolve(module, "clFinish", api.Finish, error)
            || !resolve(module, "clCreateProgramWithSource", api.CreateProgramWithSource, error)
            || !resolve(module, "clBuildProgram", api.BuildProgram, error)
            || !resolve(module, "clGetProgramBuildInfo", api.GetProgramBuildInfo, error)
            || !resolve(module, "clCreateKernel", api.CreateKernel, error)
            || !resolve(module, "clReleaseKernel", api.ReleaseKernel, error)
            || !resolve(module, "clReleaseProgram", api.ReleaseProgram, error)
            || !resolve(module, "clSetKernelArg", api.SetKernelArg, error)
            || !resolve(module, "clEnqueueNDRangeKernel", api.EnqueueNDRangeKernel, error)) {
        error = "missing OpenCL symbol: " + error;
        return false;
    }

    return true;
}

} // namespace opencl

} // namespace rtengine
