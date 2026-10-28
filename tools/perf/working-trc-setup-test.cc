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

/*
 * Focused regression test for the setup-only contract of
 * ImProcFunctions::workingtrc().
 *
 * A synthetic gradient is put through the same two calls the export pipeline
 * makes. The setup-only run must return non-zero, finite probe means and the
 * output profile, create no transform and leave the destination untouched. The
 * profile must be accepted by ICCStore::buildGpuProfile() and is closed by this
 * test. A full run over an identical input must still produce the digest
 * recorded from the pre-change CPU implementation, so the refactor is proved
 * to leave the converted pixels unchanged.
 *
 *   working-trc-setup-test <scratch-dir>
 *
 * The scratch directory holds the settings and cache; the test never writes
 * outside it.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glib.h>
#include <giomm.h>
#include <glibmm.h>

#include "config.h"

#include "rtengine/iccstore.h"
#include "rtengine/imagefloat.h"
#include "rtengine/improcfun.h"
#include "rtengine/procparams.h"
#include "rtengine/rtengine.h"
#include "rtengine/rtapp.h"
#include "rtengine/utils.h"
#include "rtgui/options.h"

namespace
{

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                         __LINE__, #expr);                                 \
            return false;                                                  \
        }                                                                  \
    } while (false)

constexpr int kWidth = 96;
constexpr int kHeight = 64;
constexpr float kStoredMaximum = 65535.f;

/*
 * Digest of the full two-pass conversion on this host, recorded with the
 * pre-change implementation. It guards the converted pixels; it is not a
 * portable colour-management reference.
 */
constexpr std::uint64_t kFullCallDigest = 0xd054eba6b2af3d09ULL;

void fillSynthetic(rtengine::Imagefloat& image)
{
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kWidth - 1);
            const float v = static_cast<float>(y) / static_cast<float>(kHeight - 1);
            image.r(y, x) = kStoredMaximum * (0.15f + 0.70f * u);
            image.g(y, x) = kStoredMaximum * (0.20f + 0.60f * (1.0f - 0.5f * u - 0.5f * v));
            image.b(y, x) = kStoredMaximum * (0.10f + 0.80f * v);
        }
    }
}

void zero(rtengine::Imagefloat& image)
{
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            image.r(y, x) = 0.f;
            image.g(y, x) = 0.f;
            image.b(y, x) = 0.f;
        }
    }
}

std::uint64_t digest(const rtengine::Imagefloat& image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xffU;
            hash *= 1099511628211ULL;
        }
    };
    for (int y = 0; y < image.getHeight(); ++y) {
        for (int x = 0; x < image.getWidth(); ++x) {
            mix(image.r(y, x));
            mix(image.g(y, x));
            mix(image.b(y, x));
        }
    }
    return hash;
}

struct CallState
{
    Glib::ustring profile;
    int illum = 0;
    int ill = 0;
    int prim = 0;
    int locprim = 0;
    int cat = 0;
    cmsHTRANSFORM transform = nullptr;
    float rdx = 0.f, rdy = 0.f, grx = 0.f, gry = 0.f, blx = 0.f, bly = 0.f;
    float meanx = 0.f, meany = 0.f, meanxe = 0.f, meanye = 0.f, maxdat = 0.f;
    double p[6] = {0., 0., 0., 0., 0., 0.};
};

void firstPass(rtengine::ImProcFunctions& ipf, rtengine::Imagefloat& src,
               rtengine::Imagefloat& dst, CallState& state)
{
    ipf.workingtrc(0, &src, &dst, kWidth, kHeight, -5, state.profile, 2.4, 12.92310,
                   0, state.ill, 0, 0, state.rdx, state.rdy, state.grx, state.gry,
                   state.blx, state.bly, state.meanx, state.meany, state.meanxe,
                   state.meanye, state.maxdat, state.p, state.transform,
                   true, false, false, false);
}

bool checkFullCall(rtengine::ImProcFunctions& ipf,
                   const rtengine::procparams::ProcParams& params,
                   float& fullMeanX, float& fullMeanY)
{
    rtengine::Imagefloat src(kWidth, kHeight);
    rtengine::Imagefloat dst(kWidth, kHeight);
    fillSynthetic(src);
    zero(dst);

    CallState state;
    state.profile = params.icm.workingProfile;
    state.illum = rtengine::toUnderlying(params.icm.will);
    state.prim = rtengine::toUnderlying(params.icm.wprim);
    state.cat = rtengine::toUnderlying(params.icm.wcat);

    firstPass(ipf, src, dst, state);
    ipf.workingtrc(0, &src, &dst, kWidth, kHeight, 5, state.profile,
                   params.icm.wGamma, params.icm.wSlope, state.cat, state.illum,
                   state.prim, state.locprim, state.rdx, state.rdy, state.grx,
                   state.gry, state.blx, state.bly, state.meanx, state.meany,
                   state.meanxe, state.meanye, state.maxdat, state.p,
                   state.transform, false, true, true, params.icm.gamut);

    const std::uint64_t result = digest(dst);
    std::printf("full call digest %016llx (expected %016llx)\n",
                static_cast<unsigned long long>(result),
                static_cast<unsigned long long>(kFullCallDigest));
    CHECK(result == kFullCallDigest);

    CHECK(std::isfinite(state.meanx) && std::isfinite(state.meany));
    CHECK(state.meanx > 0.f && state.meany > 0.f);
    fullMeanX = state.meanx;
    fullMeanY = state.meany;

    return true;
}

bool checkSetupOnly(rtengine::ImProcFunctions& ipf,
                    const rtengine::procparams::ProcParams& params,
                    float fullMeanX, float fullMeanY)
{
    rtengine::Imagefloat src(kWidth, kHeight);
    rtengine::Imagefloat dst(kWidth, kHeight);
    fillSynthetic(src);
    zero(dst);

    CallState state;
    state.profile = params.icm.workingProfile;
    state.illum = rtengine::toUnderlying(params.icm.will);
    state.prim = rtengine::toUnderlying(params.icm.wprim);
    state.cat = rtengine::toUnderlying(params.icm.wcat);

    firstPass(ipf, src, dst, state);
    const std::uint64_t firstPassDigest = digest(dst);

    float probeMeanX = 0.f;
    float probeMeanY = 0.f;
    cmsHPROFILE profile = nullptr;

    ipf.workingtrc(0, &src, &dst, kWidth, kHeight, 5, state.profile,
                   params.icm.wGamma, params.icm.wSlope, state.cat, state.illum,
                   state.prim, state.locprim, state.rdx, state.rdy, state.grx,
                   state.gry, state.blx, state.bly, state.meanx, state.meany,
                   state.meanxe, state.meanye, state.maxdat, state.p,
                   state.transform, false, true, true, params.icm.gamut,
                   &probeMeanX, &probeMeanY, &profile, true);

    std::printf("setup-only probe means %.9g %.9g\n",
                static_cast<double>(probeMeanX),
                static_cast<double>(probeMeanY));

    // The probe must carry the same values the full call computes and must be
    // usable as a white point: non-zero and finite.
    CHECK(std::isfinite(probeMeanX) && std::isfinite(probeMeanY));
    CHECK(probeMeanX > 0.f && probeMeanY > 0.f);
    CHECK(probeMeanX == fullMeanX && probeMeanY == fullMeanY);

    // No transform, no pixel conversion: only the epsilon clamp and the
    // first pass may have touched the buffers.
    CHECK(state.transform == nullptr);
    CHECK(digest(dst) == firstPassDigest);

    CHECK(profile != nullptr);
    if (profile == nullptr) {
        return false;
    }

    float matrix[9] = {};
    std::vector<float> gamma;
    const bool gpuReady = rtengine::buildGpuProfile(profile, matrix, gamma);
    std::printf("buildGpuProfile %s, table %zu\n", gpuReady ? "ok" : "failed",
                gamma.size());
    cmsCloseProfile(profile);
    profile = nullptr;
    CHECK(gpuReady);
    CHECK(!gamma.empty());

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    Glib::init();
    Gio::init();

    // Keep the run self-contained: no settings or cache outside the scratch
    // directory. The engine requires RT_SETTINGS to be absolute.
    std::string scratchDir = argv[1];
    if (scratchDir.empty() || scratchDir[0] != '/') {
        std::fprintf(stderr, "scratch directory must be absolute: %s\n",
                     scratchDir.c_str());
        return 2;
    }
    const std::string settingsDir = scratchDir + "/settings";
    const std::string cacheDir = scratchDir + "/cache";
    g_setenv("RT_SETTINGS", settingsDir.c_str(), TRUE);
    g_setenv("RT_CACHE", cacheDir.c_str(), TRUE);

    try {
        App::get().setArgv0(DATA_SEARCH_PATH);
        Options::load(true);
    } catch (const Options::Error& error) {
        std::fprintf(stderr, "options: %s\n", error.what());
        return 2;
    }

    rtengine::procparams::ProcParams params;
    params.icm.workingProfile = "Rec2020";
    params.icm.workingTRC =
        rtengine::procparams::ColorManagementParams::WorkingTrc::GAMMA_2_2;
    params.icm.trcExp = true;

    rtengine::ImProcFunctions ipf(&params, false);

    float fullMeanX = 0.f;
    float fullMeanY = 0.f;
    if (!checkFullCall(ipf, params, fullMeanX, fullMeanY)) {
        return 1;
    }
    if (!checkSetupOnly(ipf, params, fullMeanX, fullMeanY)) {
        return 1;
    }

    std::printf("working-trc-setup-test: PASS\n");
    return 0;
}
