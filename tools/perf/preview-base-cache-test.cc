/*
 *  This file is part of RawTherapee.
 *
 *  Regression test for the opt-in preview base cache. Build it with:
 *
 *    cmake --build <build-dir> --target preview-base-cache-test
 *    <build-dir>/rtengine/preview-base-cache-test
 */
#include <cstdint>
#include <iostream>
#include <string>

#include "rtengine/imagefloat.h"
#include "rtengine/previewbasecache.h"

using rtengine::Imagefloat;
using rtengine::PreviewBaseCache;

namespace
{

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::cerr << __FILE__ << ":" << __LINE__                      \
                      << ": CHECK failed: " #expr << "\n";                \
            return false;                                                  \
        }                                                                  \
    } while (false)

void fill(Imagefloat& image, float value)
{
    for (int y = 0; y < image.getHeight(); ++y) {
        for (int x = 0; x < image.getWidth(); ++x) {
            image.r(y, x) = value;
            image.g(y, x) = value + 1.0f;
            image.b(y, x) = value + 2.0f;
        }
    }
}

bool checkImage(const Imagefloat& image, float value)
{
    for (int y = 0; y < image.getHeight(); ++y) {
        for (int x = 0; x < image.getWidth(); ++x) {
            CHECK(image.r(y, x) == value);
            CHECK(image.g(y, x) == value + 1.0f);
            CHECK(image.b(y, x) == value + 2.0f);
        }
    }
    return true;
}

PreviewBaseCache::Key makeKey(std::uint64_t generation)
{
    PreviewBaseCache::Key key;
    key.source = reinterpret_cast<const rtengine::ImageSource*>(0x1);
    key.sourceGeneration = generation;
    key.frame = 0;
    key.sensorType = 0;
    key.raw = true;
    key.wbTemp = 5500.0;
    key.wbGreen = 1.0;
    key.wbEqual = 1.0;
    key.transform = 0;
    key.scale = 1;
    key.fullWidth = 8;
    key.fullHeight = 8;
    key.width = 8;
    key.height = 8;
    return key;
}

bool testDefaultAndOptOut()
{
    g_unsetenv("RT_PREVIEW_BASE_CACHE");
    PreviewBaseCache enabledByDefault;
    CHECK(enabledByDefault.enabled());

    g_setenv("RT_PREVIEW_BASE_CACHE", "0", TRUE);
    PreviewBaseCache optOut;
    CHECK(!optOut.enabled());

    g_setenv("RT_PREVIEW_BASE_CACHE", "off", TRUE);
    PreviewBaseCache optOutOff;
    CHECK(!optOutOff.enabled());

    g_setenv("RT_PREVIEW_BASE_CACHE", "1", TRUE);
    PreviewBaseCache optIn;
    CHECK(optIn.enabled());

    g_unsetenv("RT_PREVIEW_BASE_CACHE");
    return true;
}

bool testDisabled()
{
    PreviewBaseCache cache(false, false);
    Imagefloat source(8, 8);
    fill(source, 10.0f);
    const auto key = makeKey(1);
    cache.store(key, &source);

    Imagefloat destination(8, 8);
    std::string reason;
    CHECK(!cache.tryRestore(key, &destination, reason));
    return true;
}

bool testStoreRestoreAndGeneration()
{
    PreviewBaseCache cache(true, false);
    Imagefloat source(8, 8);
    fill(source, 10.0f);

    const auto key = makeKey(1);
    cache.store(key, &source);

    Imagefloat destination(8, 8);
    std::string reason;
    CHECK(cache.tryRestore(key, &destination, reason));
    CHECK(checkImage(destination, 10.0f));

    auto newerSource = key;
    newerSource.sourceGeneration = 2;
    CHECK(!cache.tryRestore(newerSource, &destination, reason));

    // The key mismatch must not consume the entry.
    CHECK(cache.tryRestore(key, &destination, reason));
    return true;
}

bool testDestinationMismatchInvalidates()
{
    PreviewBaseCache cache(true, false);
    Imagefloat source(8, 8);
    fill(source, 20.0f);
    const auto key = makeKey(1);
    cache.store(key, &source);

    Imagefloat wrongSize(4, 4);
    std::string reason;
    CHECK(!cache.tryRestore(key, &wrongSize, reason));

    Imagefloat destination(8, 8);
    CHECK(!cache.tryRestore(key, &destination, reason));
    return true;
}

bool testDirtyAndClear()
{
    PreviewBaseCache cache(true, false);
    Imagefloat source(8, 8);
    fill(source, 30.0f);
    const auto key = makeKey(1);
    cache.store(key, &source);
    cache.markDirty("test-invalidate");

    Imagefloat destination(8, 8);
    std::string reason;
    CHECK(!cache.tryRestore(key, &destination, reason));
    CHECK(reason.find("test-invalidate") != std::string::npos);

    cache.store(key, &source);
    cache.clear("test-clear");
    CHECK(!cache.tryRestore(key, &destination, reason));
    return true;
}

bool testSizeBound()
{
    CHECK(PreviewBaseCache::fits(1, 1));
    CHECK(PreviewBaseCache::fits(1024, 1024));
    CHECK(!PreviewBaseCache::fits(0, 1));
    CHECK(!PreviewBaseCache::fits(1, 0));
    CHECK(!PreviewBaseCache::fits(1, static_cast<int>(PreviewBaseCache::MAX_BYTES)));
    return true;
}

} // namespace

int main()
{
    if (!testDefaultAndOptOut()
            || !testDisabled()
            || !testStoreRestoreAndGeneration()
            || !testDestinationMismatchInvalidates()
            || !testDirtyAndClear()
            || !testSizeBound()) {
        std::cout << "preview-base-cache-test: FAIL\n";
        return 1;
    }

    std::cout << "preview-base-cache-test: PASS\n";
    return 0;
}
