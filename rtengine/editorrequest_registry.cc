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

#include "editorrequest_registry.h"

#include <cassert>
#include <cstdlib>

namespace rtengine
{

namespace
{

struct StaticEventRange {
    int first;
    int last;
    RequestIntent intent;
};

/*
 * Recorded request classification of the static ProcEventCode identities.
 *
 * Each row is a contiguous run of events whose action names the same request
 * families. The table is data about the events the GUI already emits; it is
 * not a request mask and it does not allocate execution payload.
 */
constexpr StaticEventRange STATIC_EVENT_RANGES[] = {
    {   0,    0, RequestIntent::DarkFrame},
    {   1,    1, RequestIntent::NoRequest},
    {   2,    3, RequestIntent::DarkFrame},
    {   4,   12, RequestIntent::AutoExposure},
    {  13,   14, RequestIntent::LuminanceCurve},
    {  15,   17, RequestIntent::NoRequest},
    {  18,   18, RequestIntent::LuminanceCurve},
    {  19,   32, RequestIntent::DirPyrEqualizer},
    {  33,   33, RequestIntent::Hdr},
    {  34,   34, RequestIntent::DarkFrame},
    {  35,   35, RequestIntent::Hdr},
    {  36,   36, RequestIntent::ControlOnly},
    {  37,   39, RequestIntent::WhiteBalance},
    {  40,   42, RequestIntent::AutoExposure},
    {  43,   46, RequestIntent::NoRequest},
    {  47,   48, RequestIntent::AllNoRaw},
    {  49,   53, RequestIntent::LuminanceCurve},
    {  54,   54, RequestIntent::NoRequest},
    {  55,   55, RequestIntent::LuminanceCurve},
    {  56,   58, RequestIntent::AllNoRaw},
    {  59,   61, RequestIntent::Hdr},
    {  62,   62, RequestIntent::DarkFrame},
    {  63,   63, RequestIntent::Crop},
    {  64,   64, RequestIntent::Hdr},
    {  65,   65, RequestIntent::AllNoRaw},
    {  66,   66, RequestIntent::NoRequest},
    {  67,   68, RequestIntent::Demosaic},
    {  69,   69, RequestIntent::Monitor},
    {  70,   70, RequestIntent::AllNoRaw},
    {  71,   71, RequestIntent::Hdr},
    {  72,   72, RequestIntent::AutoExposure},
    {  73,   76, RequestIntent::ControlOnly},
    {  77,   77, RequestIntent::NoRequest},
    {  78,   80, RequestIntent::ControlOnly},
    {  81,   81, RequestIntent::DarkFrame},
    {  82,   82, RequestIntent::NoRequest},
    {  83,   83, RequestIntent::Hdr},
    {  84,   84, RequestIntent::DarkFrame},
    {  85,   85, RequestIntent::AutoExposure},
    {  86,   87, RequestIntent::DirPyrEqualizer},
    {  88,   93, RequestIntent::AllNoRaw},
    {  94,   96, RequestIntent::LuminanceCurve},
    {  97,   97, RequestIntent::Demosaic},
    {  98,   98, RequestIntent::DarkFrame},
    {  99,  103, RequestIntent::AutoExposure},
    { 104,  106, RequestIntent::DirPyrEqualizer},
    { 107,  107, RequestIntent::AutoExposure},
    { 108,  109, RequestIntent::ControlOnly},
    { 110,  112, RequestIntent::LuminanceCurve},
    { 113,  113, RequestIntent::Demosaic},
    { 114,  114, RequestIntent::AllNoRaw},
    { 115,  115, RequestIntent::Demosaic},
    { 116,  123, RequestIntent::DarkFrame},
    { 124,  124, RequestIntent::NoRequest},
    { 125,  128, RequestIntent::DarkFrame},
    { 129,  129, RequestIntent::Hdr},
    { 130,  135, RequestIntent::NoRequest},
    { 136,  140, RequestIntent::DarkFrame},
    { 141,  148, RequestIntent::DirPyrEqualizer},
    { 149,  149, RequestIntent::Demosaic},
    { 150,  156, RequestIntent::AutoExposure},
    { 157,  161, RequestIntent::DirPyrEqualizer},
    { 162,  165, RequestIntent::AutoExposure},
    { 166,  166, RequestIntent::DarkFrame},
    { 167,  168, RequestIntent::LuminanceCurve},
    { 169,  169, RequestIntent::AutoExposure},
    { 170,  171, RequestIntent::LuminanceCurve},
    { 172,  172, RequestIntent::AllNoRaw},
    { 173,  199, RequestIntent::LuminanceCurve},
    { 200,  202, RequestIntent::AllNoRaw},
    { 203,  203, RequestIntent::Demosaic},
    { 204,  205, RequestIntent::LuminanceCurve},
    { 206,  206, RequestIntent::DirPyrEqualizer},
    { 207,  207, RequestIntent::WhiteBalance},
    { 208,  208, RequestIntent::NoRequest},
    { 209,  212, RequestIntent::Hdr},
    { 213,  234, RequestIntent::AutoExposure},
    { 235,  235, RequestIntent::NoRequest},
    { 236,  236, RequestIntent::AutoExposure},
    { 237,  244, RequestIntent::Hdr},
    { 245,  247, RequestIntent::LuminanceCurve},
    { 248,  248, RequestIntent::AllNoRaw},
    { 249,  249, RequestIntent::NoRequest},
    { 250,  250, RequestIntent::AutoExposure},
    { 251,  255, RequestIntent::AllNoRaw},
    { 256,  272, RequestIntent::AutoExposure},
    { 273,  274, RequestIntent::NoRequest},
    { 275,  283, RequestIntent::AutoExposure},
    { 284,  286, RequestIntent::AllNoRaw},
    { 287,  291, RequestIntent::DarkFrame},
    { 292,  294, RequestIntent::AutoExposure},
    { 295,  296, RequestIntent::AllNoRaw},
    { 297,  297, RequestIntent::DarkFrame},
    { 298,  298, RequestIntent::AllNoRaw},
    { 299,  299, RequestIntent::NoRequest},
    { 300,  302, RequestIntent::AllNoRaw},
    { 303,  357, RequestIntent::DirPyrEqualizer},
    { 358,  358, RequestIntent::DarkFrame},
    { 359,  369, RequestIntent::DirPyrEqualizer},
    { 370,  383, RequestIntent::ControlOnly},
    { 384,  391, RequestIntent::DirPyrEqualizer},
    { 392,  394, RequestIntent::AllNoRaw},
    { 395,  405, RequestIntent::DirPyrEqualizer},
    { 406,  406, RequestIntent::Demosaic},
    { 407,  407, RequestIntent::Retinex},
    { 408,  408, RequestIntent::NoRequest},
    { 409,  412, RequestIntent::Retinex},
    { 413,  413, RequestIntent::Demosaic},
    { 414,  414, RequestIntent::Retinex},
    { 415,  415, RequestIntent::Demosaic},
    { 416,  417, RequestIntent::Retinex},
    { 418,  422, RequestIntent::Demosaic},
    { 423,  423, RequestIntent::Retinex},
    { 424,  424, RequestIntent::NoRequest},
    { 425,  425, RequestIntent::Demosaic},
    { 426,  427, RequestIntent::Monitor},
    { 428,  436, RequestIntent::Retinex},
    { 437,  438, RequestIntent::Demosaic},
    { 439,  439, RequestIntent::AllNoRaw},
    { 440,  441, RequestIntent::Retinex},
    { 442,  442, RequestIntent::Monitor},
    { 443,  443, RequestIntent::WhiteBalance},
    { 444,  444, RequestIntent::DarkFrame},
    { 445,  447, RequestIntent::NoRequest},
    { 448,  448, RequestIntent::Demosaic},
    { 449,  450, RequestIntent::NoRequest},
    { 451,  452, RequestIntent::Demosaic},
    { 453,  455, RequestIntent::NoRequest},
    { 456,  456, RequestIntent::Demosaic},
    { 457,  460, RequestIntent::NoRequest},
    { 461,  461, RequestIntent::Demosaic},
    { 462,  462, RequestIntent::NoRequest},
    { 463,  464, RequestIntent::Demosaic},
    { 465,  466, RequestIntent::NoRequest},
    { 467,  468, RequestIntent::Demosaic},
    { 469,  469, RequestIntent::NoRequest},
    { 470,  471, RequestIntent::Demosaic},
    { 472,  472, RequestIntent::NoRequest},
    { 473,  474, RequestIntent::Demosaic},
    { 475,  483, RequestIntent::LuminanceCurve},
    { 484,  486, RequestIntent::DarkFrame},
    { 487,  487, RequestIntent::AllNoRaw},
    { 488,  489, RequestIntent::Hdr},
    { 490,  490, RequestIntent::WhiteBalance},
    { 491,  491, RequestIntent::AutoExposure},
    { 492,  492, RequestIntent::LuminanceCurve},
    { 493,  493, RequestIntent::Demosaic},
    { 494,  494, RequestIntent::CaptureSharpen},
    { 495,  495, RequestIntent::AutoExposure},
    { 496,  496, RequestIntent::Hdr},
    { 497,  498, RequestIntent::ControlOnly},
    { 499,  602, RequestIntent::AutoExposure},
    { 603,  603, RequestIntent::NoRequest},
    { 604,  662, RequestIntent::AutoExposure},
    { 663,  663, RequestIntent::NoRequest},
    { 664,  672, RequestIntent::AutoExposure},
    { 673,  673, RequestIntent::ControlOnly},
    { 674,  844, RequestIntent::AutoExposure},
    { 845,  845, RequestIntent::Hdr},
    { 846,  846, RequestIntent::AutoExposure},
    { 847,  847, RequestIntent::NoRequest},
    { 848,  848, RequestIntent::Hdr},
    { 849,  922, RequestIntent::AutoExposure},
    { 923,  923, RequestIntent::NoRequest},
    { 924, 1057, RequestIntent::AutoExposure},
    {1058, 1060, RequestIntent::Hdr},
    {1061, 1080, RequestIntent::AutoExposure},
    {1081, 1081, RequestIntent::Hdr},
    {1082, 1148, RequestIntent::AutoExposure},
    {1149, 1149, RequestIntent::NoRequest},
};

[[noreturn]] void failUnknownEvent(int code)
{
    assert(false && "unknown ProcEvent has no typed request");
    (void)code;
    std::abort();
}

RequestIntent staticIntentFor(int code)
{
    if (code < 0 || code >= NUMOFEVENTS) {
        failUnknownEvent(code);
    }

    for (const StaticEventRange& range : STATIC_EVENT_RANGES) {
        if (code >= range.first && code <= range.last) {
            return range.intent;
        }
    }

    failUnknownEvent(code);
}

} // namespace

EditorRequestRegistry::EditorRequestRegistry()
    : nextEvent_(NUMOFEVENTS)
{
}

EditorRequestRegistry *EditorRequestRegistry::getInstance()
{
    static EditorRequestRegistry instance;
    return &instance;
}

RequestIntent EditorRequestRegistry::staticEventIntent(int code)
{
    return staticIntentFor(code);
}

ProcEvent EditorRequestRegistry::newEvent()
{
    return ProcEvent(++nextEvent_);
}

void EditorRequestRegistry::registerRequest(ProcEvent event,
                                            const EditorRequest& request)
{
    requests_[int(event)] = request;
}

bool EditorRequestRegistry::hasRegisteredRequest(ProcEvent event) const
{
    return requests_.count(int(event)) != 0;
}

EditorRequest EditorRequestRegistry::requestFor(ProcEvent event) const
{
    const int code = int(event);
    const auto it = requests_.find(code);

    if (it != requests_.end()) {
        return it->second;
    }

    return make_request(staticIntentFor(code));
}

} // namespace rtengine
