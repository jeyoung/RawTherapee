#include "stage_cache_integration.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace rtengine::stage
{

namespace
{

constexpr const char* NODE_CACHE_SWITCH = "RT_NODE_CACHE";
constexpr const char* NODE_CACHE_BUDGET = "RT_NODE_CACHE_MB";
constexpr const char* NODE_CACHE_STATS = "RT_NODE_CACHE_STATS";

/* Default budget, in MiB. Documented in devnotes/perf/STAGE_PIPELINE.md. */
constexpr size_t DEFAULT_BUDGET_MIB = 256;
/* Valid budget range, in MiB. Values outside it fall back to the default. */
constexpr size_t MIN_BUDGET_MIB = 1;
constexpr size_t MAX_BUDGET_MIB = 8192;

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

void appendHex(std::string& out, uint64_t value)
{
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(value));
    out.append(buffer, 16);
}

void appendDecimal(std::string& out, int64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld",
                  static_cast<long long>(value));
    out.append(buffer);
}

void appendField(std::string& out, const std::string& value)
{
    // Canonical block ids contain no unit separator, and the fingerprints are
    // fixed-width hexadecimal, so the separator cannot be forged.
    out.append(value);
    out.push_back('\x1f');
}

uint64_t digestOf(const std::string& text)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Process-wide snapshot for the on-demand diagnostic. */
std::mutex& reportMutex()
{
    static std::mutex mutex;
    return mutex;
}

StageCacheStats& reportSnapshot()
{
    static StageCacheStats snapshot;
    return snapshot;
}

bool& reportRegistered()
{
    static bool registered = false;
    return registered;
}

bool& reportEnabled()
{
    static bool enabled = std::getenv(NODE_CACHE_STATS) != nullptr;
    return enabled;
}

void reportAtExit()
{
    StageCacheStats snapshot;
    {
        std::lock_guard<std::mutex> lock(reportMutex());
        snapshot = reportSnapshot();
    }
    std::fprintf(
        stderr,
        "node-cache: budget=%llu used=%llu hits=%llu misses=%llu "
        "refused=%llu inserts=%llu rejects=%llu evictions=%llu "
        "bytes-in=%llu bytes-evict=%llu\n",
        static_cast<unsigned long long>(snapshot.bytes_budget),
        static_cast<unsigned long long>(snapshot.bytes_used),
        static_cast<unsigned long long>(snapshot.hits),
        static_cast<unsigned long long>(snapshot.misses),
        static_cast<unsigned long long>(snapshot.refused),
        static_cast<unsigned long long>(snapshot.inserts),
        static_cast<unsigned long long>(snapshot.rejects),
        static_cast<unsigned long long>(snapshot.evictions),
        static_cast<unsigned long long>(snapshot.bytes_inserted),
        static_cast<unsigned long long>(snapshot.bytes_evicted));
}

void publish(const StageCacheStats& stats)
{
    if (!reportEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(reportMutex());
    reportSnapshot() = stats;
}

void writeU32(std::vector<unsigned char>& out, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
    }
}

void writeU64(std::vector<unsigned char>& out, uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xffu));
    }
}

bool readU64(const unsigned char* data, size_t size, size_t& offset,
             uint64_t& value)
{
    if (offset + 8 > size) {
        return false;
    }
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(data[offset++]) << shift;
    }
    return true;
}

bool readU32(const unsigned char* data, size_t size, size_t& offset,
             uint32_t& value)
{
    if (offset + 4 > size) {
        return false;
    }
    value = static_cast<uint32_t>(data[offset])
          | (static_cast<uint32_t>(data[offset + 1]) << 8)
          | (static_cast<uint32_t>(data[offset + 2]) << 16)
          | (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

uint64_t doubleBits(double value)
{
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double is 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double bitsDouble(uint64_t bits)
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

constexpr unsigned char AUX_MAGIC[3] = {'N', 'C', 'A'};
constexpr unsigned char AUX_VERSION = 1;

} // namespace

bool NodeCacheKey::operator==(const NodeCacheKey& other) const
{
    return node == other.node && render == other.render &&
           block_ids == other.block_ids &&
           block_values == other.block_values && work == other.work &&
           width == other.width && height == other.height &&
           planes == other.planes && format == other.format;
}

StageKey nodeCacheStageKey(const NodeCacheKey& key)
{
    std::string encoded;
    encoded.reserve(64 + key.block_ids.size() * 48);
    appendField(encoded, key.node);
    appendHex(encoded, key.render.words[0]);
    appendHex(encoded, key.render.words[1]);
    encoded.push_back('\x1f');
    for (size_t i = 0; i < key.block_ids.size(); ++i) {
        appendField(encoded, key.block_ids[i]);
        appendHex(encoded, key.block_values[i].words[0]);
        appendHex(encoded, key.block_values[i].words[1]);
        encoded.push_back('\x1f');
    }
    appendHex(encoded, key.work);
    encoded.push_back('\x1f');
    appendDecimal(encoded, key.width);
    encoded.push_back('\x1f');
    appendDecimal(encoded, key.height);
    encoded.push_back('\x1f');
    appendDecimal(encoded, key.planes);
    encoded.push_back('\x1f');
    appendDecimal(encoded, key.format);
    encoded.push_back('\x1f');

    StageKey stage;
    stage.stage = std::move(encoded);
    stage.params = digestOf(stage.stage);
    stage.inputs[0] = key.render.words[0];
    stage.inputs[1] = key.render.words[1];
    stage.inputs[2] = key.work;
    stage.level = static_cast<uint32_t>(key.width);
    stage.consumer = key.format;
    return stage;
}

uint64_t nodeCacheKeyDigest(const NodeCacheKey& key)
{
    return nodeCacheStageKey(key).params;
}

uint64_t nodeCacheWorkBits(const WorkOrder& work)
{
    uint64_t bits = 0;
    for (size_t i = 0; i < workItemCount; ++i) {
        if (work.has(static_cast<WorkItem>(i))) {
            bits |= 1ULL << i;
        }
    }
    return bits;
}

std::vector<unsigned char> encodeRawBaseTierState(const RawBaseTierState& state)
{
    std::vector<unsigned char> out;
    out.reserve(64 + state.wb_method.size() + state.histogram.size() * 4);
    for (unsigned char byte : AUX_MAGIC) {
        out.push_back(byte);
    }
    out.push_back(AUX_VERSION);
    writeU64(out, doubleBits(state.wb_temp));
    writeU64(out, doubleBits(state.wb_green));
    writeU64(out, doubleBits(state.wb_equal));
    writeU64(out, static_cast<uint64_t>(
                     static_cast<uint32_t>(state.wb_observer)));
    writeU64(out, static_cast<uint64_t>(state.wb_method.size()));
    out.insert(out.end(), state.wb_method.begin(), state.wb_method.end());
    out.push_back(state.denoise_valid ? 1u : 0u);
    writeU64(out, static_cast<uint64_t>(state.histogram.size()));
    for (uint32_t value : state.histogram) {
        writeU32(out, value);
    }
    return out;
}

bool decodeRawBaseTierState(const unsigned char* data, size_t size,
                            RawBaseTierState& state)
{
    if (data == nullptr || size < 4 ||
        std::memcmp(data, AUX_MAGIC, sizeof(AUX_MAGIC)) != 0 ||
        data[3] != AUX_VERSION) {
        return false;
    }
    size_t offset = 4;
    uint64_t temp = 0;
    uint64_t green = 0;
    uint64_t equal = 0;
    uint64_t observer = 0;
    uint64_t methodSize = 0;
    if (!readU64(data, size, offset, temp) ||
        !readU64(data, size, offset, green) ||
        !readU64(data, size, offset, equal) ||
        !readU64(data, size, offset, observer) ||
        !readU64(data, size, offset, methodSize) ||
        methodSize > size || offset + methodSize + 1 > size) {
        return false;
    }
    RawBaseTierState decoded;
    decoded.wb_temp = bitsDouble(temp);
    decoded.wb_green = bitsDouble(green);
    decoded.wb_equal = bitsDouble(equal);
    decoded.wb_observer = static_cast<int32_t>(
        static_cast<uint32_t>(observer));
    decoded.wb_method.assign(
        reinterpret_cast<const char*>(data + offset),
        static_cast<size_t>(methodSize));
    offset += static_cast<size_t>(methodSize);
    decoded.denoise_valid = data[offset++] != 0;
    uint64_t count = 0;
    if (!readU64(data, size, offset, count) ||
        count > (size - offset) / 4) {
        return false;
    }
    decoded.histogram.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t value = 0;
        if (!readU32(data, size, offset, value)) {
            return false;
        }
        decoded.histogram.push_back(value);
    }
    if (offset != size) {
        return false;
    }
    state = std::move(decoded);
    return true;
}

bool nodeCacheValueEnables(const char* value)
{
    if (value == nullptr) {
        return false;
    }
    if (value[0] == '0' && value[1] == '\0') {
        return false;
    }
    return !(lower(value[0]) == 'o' && lower(value[1]) == 'f'
             && lower(value[2]) == 'f' && value[3] == '\0');
}

bool nodeCacheEnabled()
{
    static const bool enabled = nodeCacheValueEnables(
        std::getenv(NODE_CACHE_SWITCH));
    return enabled;
}

size_t nodeCacheBudgetMiB()
{
    static const size_t mib = [] {
        const char* raw = std::getenv(NODE_CACHE_BUDGET);
        if (raw == nullptr || raw[0] == '\0') {
            return DEFAULT_BUDGET_MIB;
        }
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(raw, &end, 10);
        if (end == raw || *end != '\0' ||
            parsed < MIN_BUDGET_MIB || parsed > MAX_BUDGET_MIB) {
            return DEFAULT_BUDGET_MIB;
        }
        return static_cast<size_t>(parsed);
    }();
    return mib;
}

size_t nodeCacheBudgetBytes()
{
    return nodeCacheBudgetMiB() * 1024u * 1024u;
}

bool nodeCacheReuseAllowed(const WorkOrder& work)
{
    return !work.has(WorkItem::sourceSelect)
        && !work.has(WorkItem::rawPreprocess)
        && !work.has(WorkItem::rawDemosaic)
        && !work.has(WorkItem::retinexPrepare);
}

bool nodeCacheSubstitutionAllowed(const WorkOrder& work)
{
    return work.has(WorkItem::rawBaseRebuild);
}

NodeCache::NodeCache()
    : NodeCache(nodeCacheEnabled(), nodeCacheBudgetBytes())
{
}

NodeCache::NodeCache(bool enabled, size_t budget_bytes)
{
    enabled_ = enabled;
    if (enabled_) {
        cache_.set_budget(budget_bytes);
    }
    if (reportEnabled() && !reportRegistered()) {
        reportRegistered() = true;
        std::atexit(reportAtExit);
    }
}

bool NodeCache::contains(const NodeCacheKey& key) const
{
    return enabled_ && cache_.contains(nodeCacheStageKey(key));
}

bool NodeCache::probe(const NodeCacheKey& key)
{
    if (!enabled_) {
        return false;
    }
    const bool found = cache_.peek(nodeCacheStageKey(key));
    publish(cache_.stats());
    return found;
}

bool NodeCache::restore(const NodeCacheKey& key, RtStageBuffer& destination,
                        std::vector<unsigned char>* aux) const
{
    if (!enabled_) {
        return false;
    }
    return cache_.fetch(nodeCacheStageKey(key), destination, aux);
}

bool NodeCache::store(const NodeCacheKey& key, const RtStageBuffer& source,
                      const void* aux, size_t aux_bytes)
{
    if (!enabled_) {
        return false;
    }
    const bool stored =
        cache_.put(nodeCacheStageKey(key), source, aux, aux_bytes);
    publish(cache_.stats());
    return stored;
}

void NodeCache::note_refused()
{
    if (!enabled_) {
        return;
    }
    cache_.note_refused();
    publish(cache_.stats());
}

size_t NodeCache::used() const
{
    return enabled_ ? cache_.used() : 0;
}

size_t NodeCache::budget() const
{
    return enabled_ ? cache_.budget() : 0;
}

StageCacheStats NodeCache::stats() const
{
    return enabled_ ? cache_.stats() : StageCacheStats{};
}

void NodeCache::reset_stats()
{
    if (enabled_) {
        cache_.reset_stats();
        publish(cache_.stats());
    }
}

void NodeCache::clear()
{
    if (enabled_) {
        cache_.clear();
        publish(cache_.stats());
    }
}

} // namespace rtengine::stage
