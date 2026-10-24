#include "stage_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

/* Owns a unique, empty directory below the system temporary directory. */
class TempRoot
{
public:
    TempRoot()
    {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = base / ("rt-stage-store-" + std::to_string(++counter_) + "-" +
                        std::to_string(stamp));
        std::filesystem::create_directories(path_, error);
    }

    ~TempRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    inline static unsigned counter_ = 0;
    std::filesystem::path path_;
};

std::string hex_key(uint64_t seed)
{
    char digits[17];
    std::snprintf(digits, sizeof(digits), "%016llx",
                  static_cast<unsigned long long>(seed));
    std::string key;
    key.reserve(rtengine::stage::StageStore::key_length);
    while (key.size() < rtengine::stage::StageStore::key_length) {
        key += digits;
    }
    key.resize(rtengine::stage::StageStore::key_length);
    return key;
}

size_t pixel_bytes(RtStageFormat format)
{
    switch (format) {
    case RT_STAGE_F32:
        return sizeof(float);
    case RT_STAGE_U16:
        return sizeof(uint16_t);
    case RT_STAGE_U8:
        return sizeof(uint8_t);
    }
    return 0;
}

size_t stored_row_bytes(const RtStageBuffer& buffer)
{
    const size_t components =
        (buffer.format == RT_STAGE_U8 && buffer.planes == 1) ? 3u : 1u;
    return static_cast<size_t>(buffer.width) * pixel_bytes(buffer.format) *
           components;
}

struct Buffer {
    std::vector<unsigned char> storage;
    RtStageBuffer view{};
};

Buffer make_buffer(int width, int height, uint32_t planes, RtStageFormat format,
                   size_t row_padding, size_t plane_padding, unsigned char fill)
{
    Buffer buffer;
    const size_t row = static_cast<size_t>(width) * pixel_bytes(format) *
                       ((format == RT_STAGE_U8 && planes == 1) ? 3u : 1u);
    const size_t row_pitch = row + row_padding;
    const size_t plane_pitch =
        row_pitch * static_cast<size_t>(height) + plane_padding;
    buffer.storage.assign(plane_pitch * (planes - 1) +
                              row_pitch * static_cast<size_t>(height),
                          fill);
    buffer.view.size = sizeof(RtStageBuffer);
    buffer.view.data = buffer.storage.data();
    buffer.view.width = width;
    buffer.view.height = height;
    buffer.view.row_pitch = row_pitch;
    buffer.view.plane_pitch = plane_pitch;
    buffer.view.planes = planes;
    buffer.view.format = format;
    buffer.view.flags = RT_STAGE_BUFFER_READ | RT_STAGE_BUFFER_WRITE;
    return buffer;
}

void fill_pattern(Buffer& buffer, uint32_t seed)
{
    const size_t row = stored_row_bytes(buffer.view);
    uint32_t value = seed * 2654435761u;
    for (uint32_t plane = 0; plane < buffer.view.planes; ++plane) {
        unsigned char* base = buffer.storage.data() +
                              static_cast<size_t>(plane) * buffer.view.plane_pitch;
        for (int y = 0; y < buffer.view.height; ++y) {
            unsigned char* cursor =
                base + static_cast<size_t>(y) * buffer.view.row_pitch;
            for (size_t i = 0; i < row; ++i) {
                value += 37u;
                cursor[i] = static_cast<unsigned char>(value & 0xffu);
            }
        }
    }
}

void copy_packed(const RtStageBuffer& from, const RtStageBuffer& to)
{
    const size_t row = stored_row_bytes(from);
    for (uint32_t plane = 0; plane < from.planes; ++plane) {
        const unsigned char* source_base =
            static_cast<const unsigned char*>(from.data) +
            static_cast<size_t>(plane) * from.plane_pitch;
        unsigned char* destination_base = static_cast<unsigned char*>(to.data) +
                                          static_cast<size_t>(plane) * to.plane_pitch;
        for (int y = 0; y < from.height; ++y) {
            std::memcpy(destination_base + static_cast<size_t>(y) * to.row_pitch,
                        source_base + static_cast<size_t>(y) * from.row_pitch,
                        row);
        }
    }
}

bool packed_equal(const RtStageBuffer& left, const RtStageBuffer& right)
{
    const size_t row = stored_row_bytes(left);
    for (uint32_t plane = 0; plane < left.planes; ++plane) {
        const unsigned char* left_base =
            static_cast<const unsigned char*>(left.data) +
            static_cast<size_t>(plane) * left.plane_pitch;
        const unsigned char* right_base =
            static_cast<const unsigned char*>(right.data) +
            static_cast<size_t>(plane) * right.plane_pitch;
        for (int y = 0; y < left.height; ++y) {
            if (std::memcmp(left_base + static_cast<size_t>(y) * left.row_pitch,
                            right_base + static_cast<size_t>(y) * right.row_pitch,
                            row) != 0) {
                return false;
            }
        }
    }
    return true;
}

bool uniform_byte(const RtStageBuffer& buffer, unsigned char& value)
{
    const size_t row = stored_row_bytes(buffer);
    value = 0;
    bool seen = false;
    for (uint32_t plane = 0; plane < buffer.planes; ++plane) {
        const unsigned char* base = static_cast<const unsigned char*>(buffer.data) +
                                    static_cast<size_t>(plane) * buffer.plane_pitch;
        for (int y = 0; y < buffer.height; ++y) {
            const unsigned char* cursor =
                base + static_cast<size_t>(y) * buffer.row_pitch;
            for (size_t i = 0; i < row; ++i) {
                if (!seen) {
                    value = cursor[i];
                    seen = true;
                } else if (cursor[i] != value) {
                    return false;
                }
            }
        }
    }
    return seen;
}

std::vector<std::filesystem::path> entry_files(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(root, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
            files.push_back(it->path());
        }
        it.increment(error);
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool all_bytes(const std::vector<unsigned char>& storage, unsigned char value)
{
    return std::all_of(storage.begin(), storage.end(),
                       [value](unsigned char byte) { return byte == value; });
}

void flip_byte(const std::filesystem::path& path, uint64_t offset_from_end)
{
    std::FILE* file = std::fopen(path.string().c_str(), "r+b");
    if (file == nullptr) {
        check(false, "corruption helper opens the entry");
        return;
    }
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (!error && size > offset_from_end) {
        const long offset = static_cast<long>(size - offset_from_end);
        if (std::fseek(file, offset, SEEK_SET) == 0) {
            const int byte = std::fgetc(file);
            if (byte != EOF && std::fseek(file, offset, SEEK_SET) == 0) {
                std::fputc(byte ^ 0x5a, file);
            }
        }
    }
    std::fclose(file);
}

} // namespace

int main()
{
    using namespace rtengine::stage;

    TempRoot temp;
    StageStore store(temp.path().string());

    constexpr int width = 13;
    constexpr int height = 7;
    const StageStoreMetadata metadata{"lanczos3", "1"};

    constexpr uint64_t entry_header_bytes = 64;

    struct Case {
        RtStageFormat format;
        uint32_t planes;
        const char* label;
    };
    const Case cases[] = {
        {RT_STAGE_F32, 1, "f32 single plane"},
        {RT_STAGE_F32, 3, "f32 three planes"},
        {RT_STAGE_U16, 1, "u16 single plane"},
        {RT_STAGE_U16, 3, "u16 three planes"},
        {RT_STAGE_U8, 1, "u8 packed single plane"},
        {RT_STAGE_U8, 3, "u8 three planes"},
    };

    uint64_t seed = 1;
    for (const Case& item : cases) {
        const std::string key = hex_key(seed);
        Buffer source = make_buffer(width, height, item.planes, item.format, 12, 16, 0);
        fill_pattern(source, static_cast<uint32_t>(seed * 7));

        Buffer destination =
            make_buffer(width, height, item.planes, item.format, 8, 24, 0xcd);
        Buffer expected =
            make_buffer(width, height, item.planes, item.format, 8, 24, 0xcd);
        copy_packed(source.view, expected.view);

        const std::string prefix = std::string(item.label) + ": ";
        check(store.put(key, source.view, metadata), prefix + "put");
        check(store.contains(key), prefix + "contains");

        /* The entry is header, name, version and packed payload, sharded. */
        const std::filesystem::path entry =
            temp.path() / key.substr(0, 2) / key.substr(2, 2) /
            (key + ".stage");
        std::error_code size_error;
        const uint64_t entry_size =
            std::filesystem::file_size(entry, size_error);
        const uint64_t expected_size =
            entry_header_bytes + metadata.stage.size() + metadata.version.size() +
            static_cast<uint64_t>(stored_row_bytes(source.view)) * height *
                item.planes;
        check(!size_error && entry_size == expected_size,
              prefix + "entry size matches the documented format");
        check(store.bytes() >= stored_row_bytes(source.view) *
                                   static_cast<size_t>(height) *
                                   item.planes,
              prefix + "bytes counts the payload");

        StageStoreMetadata read;
        check(store.get(key, destination.view, read), prefix + "get");
        check(read.stage == metadata.stage && read.version == metadata.version,
              prefix + "metadata round-trips");
        check(packed_equal(source.view, destination.view),
              prefix + "packed rows round-trip exactly");
        check(destination.storage == expected.storage,
              prefix + "destination padding is untouched");

        ++seed;
    }

    /* Two-level sharding and one file per key. */
    {
        const auto files = entry_files(temp.path());
        check(files.size() == std::size(cases), "one entry file per key");
        const std::filesystem::path relative =
            files.front().lexically_relative(temp.path());
        check(std::distance(relative.begin(), relative.end()) == 3,
              "entry path has two shard levels");
        auto component = relative.begin();
        const std::string first = component->string();
        ++component;
        const std::string second = component->string();
        check(first.size() == 2 && second.size() == 2,
              "shard directory names are two digits");
        check(files.front().extension() == ".stage",
              "entry file carries the stage suffix");
    }

    /* Upper-case keys address the same entry. */
    {
        const std::string lower = hex_key(0xabcdef01);
        Buffer source = make_buffer(4, 4, 1, RT_STAGE_F32, 0, 0, 0x77);
        check(store.put(lower, source.view, metadata), "lower-case key stored");
        std::string upper = lower;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        Buffer destination = make_buffer(4, 4, 1, RT_STAGE_F32, 0, 0, 0xcd);
        check(store.get(upper, destination.view), "keys are case insensitive");
        check(store.erase(upper), "upper-case key erases the entry");
        check(!store.contains(lower), "erased entry is gone");
    }

    /* Invalid keys are rejected and never become paths. */
    {
        Buffer source = make_buffer(4, 4, 1, RT_STAGE_F32, 0, 0, 0);
        const StageStoreMetadata key_metadata{"stage", "1"};
        check(!store.put(std::string(63, 'a'), source.view, key_metadata),
              "short key rejected");
        check(!store.put(std::string(65, 'a'), source.view, key_metadata),
              "long key rejected");
        check(!store.put(std::string(64, 'z'), source.view, key_metadata),
              "non-hex key rejected");
        check(!store.put("../" + std::string(61, '0'), source.view, key_metadata),
              "traversal key rejected");
        Buffer destination = make_buffer(4, 4, 1, RT_STAGE_F32, 0, 0, 0);
        check(!store.get("../" + std::string(61, '0'), destination.view),
              "traversal key cannot be read");
        check(!store.erase(std::string(64, 'z')), "non-hex key cannot be erased");
        check(!store.contains(std::string(64, 'z')),
              "non-hex key is not contained");
    }

    /* Metadata limits. */
    {
        Buffer source = make_buffer(4, 4, 1, RT_STAGE_F32, 0, 0, 0);
        StageStoreMetadata too_long{std::string(65, 'n'), "1"};
        check(!store.put(hex_key(9), source.view, too_long),
              "over-long stage name rejected");
        StageStoreMetadata empty_name{"", "1"};
        check(!store.put(hex_key(9), source.view, empty_name),
              "empty stage name rejected");
    }

    /* Atomic overwrite leaves exactly one entry and no temporary file. */
    {
        const std::string key = hex_key(100);
        Buffer first = make_buffer(8, 4, 1, RT_STAGE_F32, 0, 0, 0x11);
        Buffer second = make_buffer(8, 4, 1, RT_STAGE_F32, 0, 0, 0x22);
        check(store.put(key, first.view, metadata), "first write stored");
        check(store.put(key, second.view, metadata), "overwrite stored");
        Buffer destination = make_buffer(8, 4, 1, RT_STAGE_F32, 0, 0, 0xcd);
        check(store.get(key, destination.view), "overwritten entry restored");
        check(all_bytes(destination.storage, 0x22),
              "overwritten payload is the later write");
        const auto files = entry_files(temp.path());
        const auto temporary =
            std::find_if(files.begin(), files.end(), [](const auto& path) {
                return path.filename().string().find(".tmp-") != std::string::npos;
            });
        check(temporary == files.end(), "no temporary file is left behind");
    }

    /* A corrupt payload byte is rejected and the destination is untouched. */
    {
        TempRoot corrupt_root;
        StageStore corrupt(corrupt_root.path().string());
        const std::string key = hex_key(200);
        Buffer source = make_buffer(16, 8, 3, RT_STAGE_F32, 0, 0, 0x33);
        check(corrupt.put(key, source.view, metadata), "corrupt fixture stored");
        const auto files = entry_files(corrupt_root.path());
        check(files.size() == 1, "corrupt fixture has one file");
        if (files.size() == 1) {
            flip_byte(files.front(), 2);
        }
        Buffer destination = make_buffer(16, 8, 3, RT_STAGE_F32, 0, 0, 0xcd);
        StageStoreMetadata read;
        check(!corrupt.get(key, destination.view, read),
              "corrupt payload rejected");
        check(all_bytes(destination.storage, 0xcd),
              "rejected entry leaves the destination untouched");
        check(corrupt.contains(key), "contains still reports the file");
    }

    /* Truncation is rejected, including a file shorter than the header. */
    {
        TempRoot truncated_root;
        StageStore truncated(truncated_root.path().string());
        const std::string key = hex_key(300);
        Buffer source = make_buffer(16, 8, 1, RT_STAGE_U16, 0, 0, 0x44);
        check(truncated.put(key, source.view, metadata), "truncate fixture stored");
        const auto files = entry_files(truncated_root.path());
        check(files.size() == 1, "truncate fixture has one file");
        if (files.size() == 1) {
            std::error_code error;
            const uint64_t size = std::filesystem::file_size(files.front(), error);
            check(!error && size > 3, "truncate fixture is large enough");
            std::filesystem::resize_file(files.front(), size - 3, error);
            check(!error, "truncate fixture resized");
            Buffer destination = make_buffer(16, 8, 1, RT_STAGE_U16, 0, 0, 0xcd);
            check(!truncated.get(key, destination.view),
                  "truncated payload rejected");
            std::filesystem::resize_file(files.front(), 16, error);
            check(!error, "truncate fixture shortened to the header");
            check(!truncated.get(key, destination.view),
                  "header-only file rejected");
        }
        check(!truncated.get(hex_key(301), source.view), "missing key rejected");
    }

    /* Geometry and pitch mismatches are rejected. */
    {
        const std::string key = hex_key(400);
        Buffer source = make_buffer(12, 6, 3, RT_STAGE_F32, 4, 8, 0);
        fill_pattern(source, 5);
        check(store.put(key, source.view, metadata), "geometry fixture stored");

        Buffer wide = make_buffer(13, 6, 3, RT_STAGE_F32, 0, 0, 0xcd);
        check(!store.get(key, wide.view), "wider destination rejected");

        Buffer short_buffer = make_buffer(12, 5, 3, RT_STAGE_F32, 0, 0, 0xcd);
        check(!store.get(key, short_buffer.view), "shorter destination rejected");

        Buffer one_plane = make_buffer(12, 6, 1, RT_STAGE_F32, 0, 0, 0xcd);
        check(!store.get(key, one_plane.view), "different plane count rejected");

        Buffer u16_destination = make_buffer(12, 6, 3, RT_STAGE_U16, 0, 0, 0xcd);
        check(!store.get(key, u16_destination.view),
              "different format rejected");

        const size_t row = stored_row_bytes(source.view);
        Buffer tight_row =
            make_buffer(12, 6, 3, RT_STAGE_F32, 0, 0, 0xcd);
        tight_row.view.row_pitch = row - 1;
        check(!store.get(key, tight_row.view), "short row pitch rejected");

        Buffer tight_plane =
            make_buffer(12, 6, 3, RT_STAGE_F32, 0, 0, 0xcd);
        tight_plane.view.plane_pitch = row * 6 - 1;
        check(!store.get(key, tight_plane.view), "short plane pitch rejected");
    }

    /* A small byte budget is enforceable through bytes(). */
    {
        const uint64_t total = store.bytes();
        uint64_t on_disk = 0;
        for (const auto& file : entry_files(temp.path())) {
            std::error_code error;
            on_disk += std::filesystem::file_size(file, error);
        }
        check(total == on_disk, "bytes() matches the files on disk");
        check(total > 0, "bytes() counts the entries");

        Buffer extra = make_buffer(32, 32, 3, RT_STAGE_F32, 0, 0, 0x55);
        const std::string extra_key = hex_key(500);
        check(store.put(extra_key, extra.view, metadata), "budget entry stored");
        check(store.bytes() > total, "store exceeds the budget");
        check(store.erase(extra_key), "budget entry erased");
        check(store.bytes() <= total, "store is back within the budget");
        check(!store.contains(extra_key), "erased entry is gone");
    }

    /* Concurrent writers do not tear an entry. */
    {
        constexpr int thread_count = 4;
        constexpr int iterations = 8;
        std::vector<char> thread_ok(thread_count, 1);
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int thread = 0; thread < thread_count; ++thread) {
            threads.emplace_back([&store, &thread_ok, thread, metadata]() {
                /* Distinct keys prove concurrent round-trips. */
                const std::string key = hex_key(600 + thread);
                const unsigned char fill =
                    static_cast<unsigned char>(0x40 + thread);
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    Buffer source =
                        make_buffer(24, 12, 3, RT_STAGE_F32, 0, 0, fill);
                    Buffer destination =
                        make_buffer(24, 12, 3, RT_STAGE_F32, 0, 0, 0xcd);
                    if (!store.put(key, source.view, metadata) ||
                        !store.get(key, destination.view) ||
                        !all_bytes(destination.storage, fill)) {
                        thread_ok[thread] = 0;
                    }
                }
                /* The shared key only needs concurrent, non-tearing writes. */
                Buffer shared = make_buffer(24, 12, 3, RT_STAGE_F32, 0, 0, fill);
                if (!store.put(hex_key(700), shared.view, metadata)) {
                    thread_ok[thread] = 0;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (int thread = 0; thread < thread_count; ++thread) {
            check(thread_ok[thread],
                  "thread " + std::to_string(thread) +
                      " round-trips its write");
        }

        Buffer destination = make_buffer(24, 12, 3, RT_STAGE_F32, 0, 0, 0xcd);
        unsigned char value = 0;
        check(store.get(hex_key(700), destination.view),
              "shared key restored after concurrent writers");
        check(uniform_byte(destination.view, value) && value >= 0x40 &&
                  value < 0x40 + thread_count,
              "shared key holds one complete writer's payload");
    }

    /* clear() removes every file and leaves a usable store. */
    {
        store.clear();
        check(store.bytes() == 0, "clear empties the store");
        check(entry_files(temp.path()).empty(), "clear removes every file");
        check(!store.contains(hex_key(600)), "clear removes entries");
        Buffer source = make_buffer(4, 4, 1, RT_STAGE_U8, 0, 0, 0x66);
        check(store.put(hex_key(700), source.view, metadata),
              "store is usable after clear");
    }

    if (failures == 0) {
        std::puts("stage-store-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
