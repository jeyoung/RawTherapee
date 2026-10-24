#include "stage_store.h"

#include "stage_buffer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  include <process.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace rtengine::stage
{

namespace
{

/*
 * On-disk layout, all integers little-endian:
 *
 *   0   char    magic[8]        "RTSTG001"
 *   8   u32     format version
 *   12  u32     fixed header size (64)
 *   16  u32     CRC32 of the fixed header with this field zeroed, followed
 *               by the stage name and version text
 *   20  u32     CRC32 of the payload
 *   24  u16     stage name length
 *   26  u16     stage version length
 *   28  i32     width
 *   32  i32     height
 *   36  u32     planes
 *   40  u32     pixel format (RtStageFormat)
 *   44  u64     packed row bytes
 *   52  u64     payload bytes
 *   60  u32     reserved, zero
 *   64  bytes   stage name, then stage version, then the packed payload
 */
constexpr char entry_magic[8] = {'R', 'T', 'S', 'T', 'G', '0', '0', '1'};
constexpr size_t fixed_header_bytes = 64;
constexpr size_t offset_magic = 0;
constexpr size_t offset_format_version = 8;
constexpr size_t offset_header_bytes = 12;
constexpr size_t offset_header_crc = 16;
constexpr size_t offset_payload_crc = 20;
constexpr size_t offset_stage_length = 24;
constexpr size_t offset_version_length = 26;
constexpr size_t offset_width = 28;
constexpr size_t offset_height = 32;
constexpr size_t offset_planes = 36;
constexpr size_t offset_format = 40;
constexpr size_t offset_row_bytes = 44;
constexpr size_t offset_payload_bytes = 52;
constexpr size_t offset_reserved = 60;

static_assert(StageStore::format_version == 1, "unexpected store format version");
static_assert(offset_reserved + sizeof(uint32_t) == fixed_header_bytes,
              "entry header layout does not fill its fixed size");
static_assert(RT_STAGE_NAME_MAX > 0, "stage names must fit the header");

/* Limits that keep a hostile or corrupt header from driving allocation. */
constexpr uint32_t max_stage_text = RT_STAGE_NAME_MAX;
constexpr uint32_t max_planes = 3;
constexpr int32_t max_dimension = 1 << 20;
constexpr uint32_t crc_chunk_bytes = 64u * 1024u;

/* A shard directory name is this many key digits. Two levels are used. */
constexpr size_t shard_digits = 2;
constexpr size_t shard_levels = 2;
constexpr const char* entry_suffix = ".stage";
constexpr const char* temp_prefix = ".tmp-";

void store_u16(unsigned char* out, uint16_t value)
{
    out[0] = static_cast<unsigned char>(value & 0xffu);
    out[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
}

void store_u32(unsigned char* out, uint32_t value)
{
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        out[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
    }
}

void store_u64(unsigned char* out, uint64_t value)
{
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        out[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
    }
}

uint16_t load_u16(const unsigned char* in)
{
    return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8));
}

uint32_t load_u32(const unsigned char* in)
{
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    }
    return value;
}

uint64_t load_u64(const unsigned char* in)
{
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

struct CrcTable {
    std::array<uint32_t, 256> values{};

    CrcTable()
    {
        constexpr uint32_t polynomial = 0xedb88320u;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (polynomial ^ (value >> 1)) : (value >> 1);
            }
            values[i] = value;
        }
    }
};

uint32_t crc32(uint32_t crc, const void* data, size_t bytes)
{
    static const CrcTable table;
    const auto* cursor = static_cast<const unsigned char*>(data);
    uint32_t value = crc ^ 0xffffffffu;
    for (size_t i = 0; i < bytes; ++i) {
        value = table.values[(value ^ cursor[i]) & 0xffu] ^ (value >> 8);
    }
    return value ^ 0xffffffffu;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool valid_format(RtStageFormat format)
{
    return format == RT_STAGE_F32 || format == RT_STAGE_U8 ||
           format == RT_STAGE_U16;
}

bool multiply_checked(uint64_t left, uint64_t right, uint64_t& product)
{
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

/* Row bytes stored for one plane, matching stage::row_bytes(). */
uint64_t packed_row_bytes(int32_t width, uint32_t planes, RtStageFormat format)
{
    if (width <= 0 || !valid_format(format)) {
        return 0;
    }
    const uint64_t pixel_bytes = bytes_per_pixel(format);
    const uint64_t components =
        (format == RT_STAGE_U8 && planes == 1) ? 3u : 1u;
    return static_cast<uint64_t>(width) * pixel_bytes * components;
}

bool safe_root(const std::string& root)
{
    if (root.empty()) {
        return false;
    }
    const std::filesystem::path path(root);
    return path.has_relative_path();
}

long process_id()
{
#ifdef _WIN32
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

FILE* open_entry(const std::filesystem::path& path, bool for_write)
{
#ifdef _WIN32
    return ::_wfopen(path.c_str(), for_write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), for_write ? "wb" : "rb");
#endif
}

bool seek_entry(FILE* file, uint64_t offset)
{
#ifdef _WIN32
    return ::_fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool flush_and_sync(FILE* file)
{
    if (std::fflush(file) != 0) {
        return false;
    }
#ifdef _WIN32
    return ::_commit(::_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

/* Best effort: make the rename durable against a power loss. */
void sync_directory(const std::filesystem::path& directory)
{
#ifndef _WIN32
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)directory;
#endif
}

struct FileCloser {
    FILE* file = nullptr;
    ~FileCloser()
    {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

struct ParsedHeader {
    int32_t width = 0;
    int32_t height = 0;
    uint32_t planes = 0;
    RtStageFormat format = RT_STAGE_F32;
    uint64_t row_bytes = 0;
    uint64_t row_extent = 0;
    uint64_t payload_bytes = 0;
    uint64_t payload_offset = 0;
    uint32_t payload_crc = 0;
    std::string stage;
    std::string version;
};

bool usable_source(const RtStageBuffer& source, uint64_t& row_bytes,
                   uint64_t& payload_bytes)
{
    if (source.size != sizeof(RtStageBuffer) || source.data == nullptr) {
        return false;
    }
    if (source.width <= 0 || source.height <= 0 ||
        source.width > max_dimension || source.height > max_dimension) {
        return false;
    }
    if (source.planes == 0 || source.planes > max_planes) {
        return false;
    }
    if (!valid_format(source.format)) {
        return false;
    }

    row_bytes = packed_row_bytes(source.width, source.planes, source.format);
    if (row_bytes == 0 ||
        static_cast<uint64_t>(source.row_pitch) < row_bytes) {
        return false;
    }

    uint64_t row_extent = 0;
    if (!multiply_checked(row_bytes, static_cast<uint64_t>(source.height),
                          row_extent)) {
        return false;
    }
    if (source.planes > 1 &&
        static_cast<uint64_t>(source.plane_pitch) < row_extent) {
        return false;
    }

    return multiply_checked(row_extent, source.planes, payload_bytes);
}

bool metadata_valid(const StageStoreMetadata& metadata)
{
    return !metadata.stage.empty() &&
           metadata.stage.size() <= max_stage_text &&
           metadata.version.size() <= max_stage_text;
}

bool write_entry(FILE* file, const RtStageBuffer& source,
                 const StageStoreMetadata& metadata, uint64_t row_bytes,
                 uint64_t payload_bytes)
{
    assert(file != nullptr);
    assert(row_bytes > 0);
    assert(payload_bytes > 0);

    uint32_t payload_crc = 0;
    for (uint32_t plane = 0; plane < source.planes; ++plane) {
        const unsigned char* base = static_cast<const unsigned char*>(source.data) +
                                    static_cast<size_t>(plane) * source.plane_pitch;
        for (int32_t y = 0; y < source.height; ++y) {
            payload_crc = crc32(payload_crc, base + static_cast<size_t>(y) *
                                                  source.row_pitch,
                                static_cast<size_t>(row_bytes));
        }
    }

    std::array<unsigned char, fixed_header_bytes> header{};
    std::memcpy(header.data() + offset_magic, entry_magic, sizeof(entry_magic));
    store_u32(header.data() + offset_format_version, StageStore::format_version);
    store_u32(header.data() + offset_header_bytes,
              static_cast<uint32_t>(fixed_header_bytes));
    store_u32(header.data() + offset_payload_crc, payload_crc);
    store_u16(header.data() + offset_stage_length,
              static_cast<uint16_t>(metadata.stage.size()));
    store_u16(header.data() + offset_version_length,
              static_cast<uint16_t>(metadata.version.size()));
    store_u32(header.data() + offset_width, static_cast<uint32_t>(source.width));
    store_u32(header.data() + offset_height, static_cast<uint32_t>(source.height));
    store_u32(header.data() + offset_planes, source.planes);
    store_u32(header.data() + offset_format, static_cast<uint32_t>(source.format));
    store_u64(header.data() + offset_row_bytes, row_bytes);
    store_u64(header.data() + offset_payload_bytes, payload_bytes);
    store_u32(header.data() + offset_reserved, 0);

    uint32_t header_crc = crc32(0, header.data(), header.size());
    header_crc = crc32(header_crc, metadata.stage.data(), metadata.stage.size());
    header_crc = crc32(header_crc, metadata.version.data(), metadata.version.size());
    store_u32(header.data() + offset_header_crc, header_crc);

    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
        return false;
    }
    if (!metadata.stage.empty() &&
        std::fwrite(metadata.stage.data(), 1, metadata.stage.size(), file) !=
            metadata.stage.size()) {
        return false;
    }
    if (!metadata.version.empty() &&
        std::fwrite(metadata.version.data(), 1, metadata.version.size(), file) !=
            metadata.version.size()) {
        return false;
    }

    for (uint32_t plane = 0; plane < source.planes; ++plane) {
        const unsigned char* base = static_cast<const unsigned char*>(source.data) +
                                    static_cast<size_t>(plane) * source.plane_pitch;
        for (int32_t y = 0; y < source.height; ++y) {
            const unsigned char* row = base + static_cast<size_t>(y) *
                                                  source.row_pitch;
            if (std::fwrite(row, 1, static_cast<size_t>(row_bytes), file) !=
                static_cast<size_t>(row_bytes)) {
                return false;
            }
        }
    }

    return true;
}

bool parse_header(FILE* file, uint64_t file_size, ParsedHeader& parsed)
{
    std::array<unsigned char, fixed_header_bytes> header{};
    if (std::fread(header.data(), 1, header.size(), file) != header.size()) {
        return false;
    }
    if (std::memcmp(header.data() + offset_magic, entry_magic,
                    sizeof(entry_magic)) != 0) {
        return false;
    }
    if (load_u32(header.data() + offset_format_version) !=
        StageStore::format_version) {
        return false;
    }
    if (load_u32(header.data() + offset_header_bytes) != fixed_header_bytes) {
        return false;
    }
    if (load_u32(header.data() + offset_reserved) != 0) {
        return false;
    }

    const uint16_t stage_length = load_u16(header.data() + offset_stage_length);
    const uint16_t version_length = load_u16(header.data() + offset_version_length);
    if (stage_length == 0 || stage_length > max_stage_text ||
        version_length > max_stage_text) {
        return false;
    }

    parsed.width = static_cast<int32_t>(load_u32(header.data() + offset_width));
    parsed.height = static_cast<int32_t>(load_u32(header.data() + offset_height));
    parsed.planes = load_u32(header.data() + offset_planes);
    parsed.format = static_cast<RtStageFormat>(load_u32(header.data() + offset_format));
    parsed.row_bytes = load_u64(header.data() + offset_row_bytes);
    parsed.payload_bytes = load_u64(header.data() + offset_payload_bytes);
    parsed.payload_crc = load_u32(header.data() + offset_payload_crc);

    if (parsed.width <= 0 || parsed.height <= 0 ||
        parsed.width > max_dimension || parsed.height > max_dimension) {
        return false;
    }
    if (parsed.planes == 0 || parsed.planes > max_planes ||
        !valid_format(parsed.format)) {
        return false;
    }

    const uint64_t expected_row =
        packed_row_bytes(parsed.width, parsed.planes, parsed.format);
    if (parsed.row_bytes != expected_row) {
        return false;
    }
    if (!multiply_checked(expected_row, static_cast<uint64_t>(parsed.height),
                          parsed.row_extent)) {
        return false;
    }
    uint64_t expected_payload = 0;
    if (!multiply_checked(parsed.row_extent, parsed.planes, expected_payload) ||
        parsed.payload_bytes != expected_payload) {
        return false;
    }

    const uint64_t names_bytes = static_cast<uint64_t>(stage_length) +
                                 static_cast<uint64_t>(version_length);
    parsed.payload_offset = static_cast<uint64_t>(fixed_header_bytes) + names_bytes;
    if (file_size != parsed.payload_offset + parsed.payload_bytes) {
        return false;
    }

    parsed.stage.resize(stage_length);
    parsed.version.resize(version_length);
    if (std::fread(parsed.stage.data(), 1, stage_length, file) != stage_length) {
        return false;
    }
    if (version_length != 0 &&
        std::fread(parsed.version.data(), 1, version_length, file) !=
            version_length) {
        return false;
    }

    std::array<unsigned char, fixed_header_bytes> copy = header;
    store_u32(copy.data() + offset_header_crc, 0);
    uint32_t header_crc = crc32(0, copy.data(), copy.size());
    header_crc = crc32(header_crc, parsed.stage.data(), parsed.stage.size());
    header_crc = crc32(header_crc, parsed.version.data(), parsed.version.size());
    return header_crc == load_u32(header.data() + offset_header_crc);
}

bool destination_fits(const RtStageBuffer& destination,
                      const ParsedHeader& parsed)
{
    if (destination.data == nullptr) {
        return false;
    }
    if (destination.width != parsed.width ||
        destination.height != parsed.height ||
        destination.planes != parsed.planes ||
        destination.format != parsed.format) {
        return false;
    }
    if (static_cast<uint64_t>(destination.row_pitch) < parsed.row_bytes) {
        return false;
    }
    if (parsed.planes > 1 &&
        static_cast<uint64_t>(destination.plane_pitch) < parsed.row_extent) {
        return false;
    }
    return true;
}

bool payload_matches(FILE* file, const ParsedHeader& parsed)
{
    std::vector<unsigned char> chunk(
        std::min<uint64_t>(crc_chunk_bytes, parsed.payload_bytes), 0);
    if (chunk.empty()) {
        return false;
    }

    uint64_t remaining = parsed.payload_bytes;
    uint32_t crc = 0;
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(remaining, chunk.size()));
        if (std::fread(chunk.data(), 1, want, file) != want) {
            return false;
        }
        crc = crc32(crc, chunk.data(), want);
        remaining -= want;
    }
    return crc == parsed.payload_crc;
}

bool copy_payload(FILE* file, const ParsedHeader& parsed,
                  const RtStageBuffer& destination)
{
    if (!seek_entry(file, parsed.payload_offset)) {
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(parsed.row_bytes);
    for (uint32_t plane = 0; plane < parsed.planes; ++plane) {
        unsigned char* base = static_cast<unsigned char*>(destination.data) +
                              static_cast<size_t>(plane) * destination.plane_pitch;
        for (int32_t y = 0; y < parsed.height; ++y) {
            unsigned char* row = base + static_cast<size_t>(y) *
                                            destination.row_pitch;
            if (std::fread(row, 1, row_bytes, file) != row_bytes) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

StageStore::StageStore(std::string root) : root_(std::move(root))
{
    assert(safe_root(root_));
    std::random_device device;
    instance_nonce_ = (static_cast<uint64_t>(device()) << 32) ^
                      static_cast<uint64_t>(device());
}

bool StageStore::valid_key(const std::string& key)
{
    if (key.size() != key_length) {
        return false;
    }
    for (char c : key) {
        if (hex_digit(c) < 0) {
            return false;
        }
    }
    return true;
}

std::string StageStore::canonical_key(const std::string& key)
{
    std::string canonical = key;
    for (char& c : canonical) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return canonical;
}

std::string StageStore::temporary_name()
{
    char nonce[17];
    std::snprintf(nonce, sizeof(nonce), "%016llx",
                  static_cast<unsigned long long>(instance_nonce_));
    return std::string(temp_prefix) + std::to_string(process_id()) + "-" +
           nonce + "-" + std::to_string(++temp_sequence_) + entry_suffix;
}

std::string StageStore::entry_path(const std::string& canonical) const
{
    assert(canonical.size() == key_length);
    std::filesystem::path path(root_);
    for (size_t level = 0; level < shard_levels; ++level) {
        path /= canonical.substr(level * shard_digits, shard_digits);
    }
    path /= canonical + entry_suffix;
    return path.string();
}

bool StageStore::put(const std::string& key, const RtStageBuffer& source,
                     const StageStoreMetadata& metadata)
{
    if (!safe_root(root_) || !valid_key(key) || !metadata_valid(metadata)) {
        return false;
    }

    uint64_t row_bytes = 0;
    uint64_t payload_bytes = 0;
    if (!usable_source(source, row_bytes, payload_bytes)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const std::filesystem::path path = entry_path(canonical_key(key));
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    const std::filesystem::path temp = path.parent_path() / temporary_name();

    FileCloser closer;
    closer.file = open_entry(temp, true);
    if (closer.file == nullptr) {
        return false;
    }

    bool ok = write_entry(closer.file, source, metadata, row_bytes, payload_bytes);
    if (ok) {
        ok = flush_and_sync(closer.file);
    }
    if (std::fclose(closer.file) != 0) {
        ok = false;
    }
    closer.file = nullptr;

    if (!ok) {
        std::filesystem::remove(temp, error);
        return false;
    }

    std::filesystem::rename(temp, path, error);
    if (error) {
        /* Some platforms refuse to replace an existing file. */
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temp, path, error);
    }
    if (error) {
        std::filesystem::remove(temp, error);
        return false;
    }

    sync_directory(path.parent_path());
    return true;
}

bool StageStore::get(const std::string& key, RtStageBuffer& destination,
                     StageStoreMetadata& metadata) const
{
    if (!safe_root(root_) || !valid_key(key)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const std::filesystem::path path = entry_path(canonical_key(key));
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    const uint64_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size < fixed_header_bytes) {
        return false;
    }

    FileCloser closer;
    closer.file = open_entry(path, false);
    if (closer.file == nullptr) {
        return false;
    }

    ParsedHeader parsed;
    if (!parse_header(closer.file, file_size, parsed)) {
        return false;
    }
    if (!destination_fits(destination, parsed)) {
        return false;
    }
    if (!payload_matches(closer.file, parsed)) {
        return false;
    }
    if (!copy_payload(closer.file, parsed, destination)) {
        return false;
    }

    metadata.stage = std::move(parsed.stage);
    metadata.version = std::move(parsed.version);
    return true;
}

bool StageStore::get(const std::string& key,
                     RtStageBuffer& destination) const
{
    StageStoreMetadata metadata;
    return get(key, destination, metadata);
}

bool StageStore::erase(const std::string& key)
{
    if (!safe_root(root_) || !valid_key(key)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    const bool removed =
        std::filesystem::remove(entry_path(canonical_key(key)), error);
    return removed && !error;
}

bool StageStore::contains(const std::string& key) const
{
    if (!safe_root(root_) || !valid_key(key)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    const bool regular =
        std::filesystem::is_regular_file(entry_path(canonical_key(key)), error);
    return regular && !error;
}

void StageStore::clear()
{
    if (!safe_root(root_)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    error.clear();
    std::filesystem::create_directories(root_, error);
}

uint64_t StageStore::bytes() const
{
    if (!safe_root(root_)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    uint64_t total = 0;
    std::filesystem::recursive_directory_iterator it(root_, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
            const uint64_t size = it->file_size(entry_error);
            if (!entry_error) {
                total += size;
            }
        }
        it.increment(error);
    }
    return total;
}

std::size_t StageStore::prune(uint64_t max_bytes)
{
    if (!safe_root(root_)) {
        return 0;
    }

    struct FileInfo {
        std::filesystem::path path;
        uint64_t bytes = 0;
        std::filesystem::file_time_type modified{};
    };

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code error;
    std::vector<FileInfo> files;
    uint64_t total = 0;

    std::filesystem::recursive_directory_iterator it(root_, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error) && !entry_error) {
            FileInfo file;
            file.path = it->path();
            file.bytes = it->file_size(entry_error);
            file.modified = it->last_write_time(entry_error);
            if (!entry_error) {
                total += file.bytes;
                files.push_back(std::move(file));
            }
        }
        it.increment(error);
    }

    if (total <= max_bytes) {
        return 0;
    }

    std::sort(files.begin(), files.end(),
              [](const FileInfo& a, const FileInfo& b) {
                  if (a.modified != b.modified) {
                      return a.modified < b.modified;
                  }
                  return a.path < b.path;
              });

    std::size_t removed = 0;
    for (const FileInfo& file : files) {
        if (total <= max_bytes) {
            break;
        }
        std::error_code remove_error;
        if (std::filesystem::remove(file.path, remove_error) && !remove_error) {
            total -= file.bytes;
            ++removed;
        }
    }
    return removed;
}

} // namespace rtengine::stage
