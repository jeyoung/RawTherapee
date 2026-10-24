#include "stage_param_hash.h"

#include <cassert>
#include <cstring>

namespace rtengine::stage
{

namespace
{

/*
 * Initial state words. The values are the fractional digits of pi and e in
 * hexadecimal; any fixed, well-spread pair works, but they must never change
 * because they define the persisted fingerprint.
 */
constexpr uint64_t kStateSeed0 = 0x243F6A8885A308D3ULL;
constexpr uint64_t kStateSeed1 = 0x13198A2E03707344ULL;

/*
 * Odd 64-bit multipliers for the compression round. Odd values keep the
 * multiply invertible modulo 2^64, so no bit of a word can be lost.
 */
constexpr uint64_t kMixMul0 = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t kMixMul1 = 0xC2B2AE3D27D4EB4FULL;

/* Mixed into the total byte length so trailing zero bytes stay significant. */
constexpr uint64_t kLengthSalt = 0x27D4EB2F165667C5ULL;

/* Separates the final avalanche from a plain compression round. */
constexpr uint64_t kFinalSalt0 = 0xA4093822299F31D0ULL;
constexpr uint64_t kFinalSalt1 = 0x082EFA98EC4E6C89ULL;

/*
 * Rotation amounts. They are coprime with 64 and not multiples of 8, so a
 * single-word change cannot stay inside one byte lane.
 */
constexpr unsigned kRotate0 = 31;
constexpr unsigned kRotate1 = 27;

/* SplitMix64 finaliser constants. */
constexpr uint64_t kAvalancheMul0 = 0xBF58476D1CE4E5B9ULL;
constexpr uint64_t kAvalancheMul1 = 0x94D049BB133111EBULL;
constexpr unsigned kAvalancheShift0 = 30;
constexpr unsigned kAvalancheShift1 = 27;
constexpr unsigned kAvalancheShift2 = 31;

constexpr uint64_t kBitsPerByte = 8;

uint64_t rotateLeft(uint64_t value, unsigned bits)
{
    assert(bits > 0 && bits < 64);
    return (value << bits) | (value >> (64 - bits));
}

/*
 * Absorb one 64-bit word into both lanes. Every word updates both lanes, so a
 * change in one field propagates forward through all later additions.
 */
void mixWord(uint64_t& state0, uint64_t& state1, uint64_t word)
{
    state0 += word;
    state0 = rotateLeft(state0, kRotate0);
    state0 *= kMixMul0;
    state1 ^= state0;
    state1 = rotateLeft(state1, kRotate1);
    state1 *= kMixMul1;
    state0 += state1;
}

uint64_t avalanche(uint64_t value)
{
    value ^= value >> kAvalancheShift0;
    value *= kAvalancheMul0;
    value ^= value >> kAvalancheShift1;
    value *= kAvalancheMul1;
    value ^= value >> kAvalancheShift2;
    return value;
}

/*
 * Canonical little-endian encoding of the low `size` bytes of `value`. The
 * fingerprint must not depend on host byte order, so every numeric field goes
 * through this helper rather than its in-memory representation.
 */
std::array<uint8_t, 8> littleEndian(uint64_t value, size_t size)
{
    assert(size > 0 && size <= 8);
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(
            (value >> (kBitsPerByte * i)) & UINT64_C(0xFF));
    }
    return bytes;
}

} // namespace

ParamHasher::ParamHasher() : state0_(kStateSeed0), state1_(kStateSeed1)
{
}

void ParamHasher::addBytes(const void* data, size_t size)
{
    assert(data != nullptr || size == 0);

    const auto* bytes = static_cast<const uint8_t*>(data);
    length_ += size;
    for (size_t i = 0; i < size; ++i) {
        pending_ |= static_cast<uint64_t>(bytes[i])
                    << (kBitsPerByte * pending_bytes_);
        ++pending_bytes_;
        if (pending_bytes_ == sizeof(pending_)) {
            mixWord(state0_, state1_, pending_);
            pending_ = 0;
            pending_bytes_ = 0;
        }
    }
}

void ParamHasher::addTagged(detail::ParamTag tag, const void* data,
                            size_t size)
{
    const uint8_t tag_byte = static_cast<uint8_t>(tag);
    addBytes(&tag_byte, 1);
    addBytes(data, size);
}

void ParamHasher::addStringBytes(detail::ParamTag tag, const void* data,
                                 size_t size)
{
    const auto length_bytes = littleEndian(size, sizeof(uint64_t));
    addTagged(tag, length_bytes.data(), length_bytes.size());
    addBytes(data, size);
}

void ParamHasher::addSectionLabel(const std::string& label)
{
    addStringBytes(detail::ParamTag::kSection, label.data(), label.size());
}

void ParamHasher::addSectionLabel(const char* label)
{
    assert(label != nullptr);
    addStringBytes(detail::ParamTag::kSection, label, std::strlen(label));
}

void ParamHasher::add(bool value)
{
    const uint8_t byte = value ? 1u : 0u;
    addTagged(detail::ParamTag::kBool, &byte, 1);
}

void ParamHasher::add(int32_t value)
{
    const auto bytes = littleEndian(static_cast<uint32_t>(value),
                                    sizeof(uint32_t));
    addTagged(detail::ParamTag::kInt32, bytes.data(), bytes.size());
}

void ParamHasher::add(int64_t value)
{
    const auto bytes = littleEndian(static_cast<uint64_t>(value),
                                    sizeof(uint64_t));
    addTagged(detail::ParamTag::kInt64, bytes.data(), bytes.size());
}

void ParamHasher::add(uint32_t value)
{
    const auto bytes = littleEndian(value, sizeof(uint32_t));
    addTagged(detail::ParamTag::kUInt32, bytes.data(), bytes.size());
}

void ParamHasher::add(uint64_t value)
{
    const auto bytes = littleEndian(value, sizeof(uint64_t));
    addTagged(detail::ParamTag::kUInt64, bytes.data(), bytes.size());
}

void ParamHasher::add(float value)
{
    static_assert(sizeof(float) == sizeof(uint32_t),
                  "float must be 32 bits for a stable fingerprint");
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto bytes = littleEndian(bits, sizeof(bits));
    addTagged(detail::ParamTag::kFloat, bytes.data(), bytes.size());
}

void ParamHasher::add(double value)
{
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "double must be 64 bits for a stable fingerprint");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto bytes = littleEndian(bits, sizeof(bits));
    addTagged(detail::ParamTag::kDouble, bytes.data(), bytes.size());
}

void ParamHasher::addString(const std::string& value)
{
    addStringBytes(detail::ParamTag::kString, value.data(), value.size());
}

void ParamHasher::addString(const Glib::ustring& value)
{
    /* ustring::bytes() is the encoded length; data() is the UTF-8 buffer. */
    addStringBytes(detail::ParamTag::kString, value.data(), value.bytes());
}

void ParamHasher::addString(const char* value)
{
    assert(value != nullptr);
    addStringBytes(detail::ParamTag::kString, value, std::strlen(value));
}

ParamFingerprint ParamHasher::finish() const
{
    uint64_t h0 = state0_;
    uint64_t h1 = state1_;

    if (pending_bytes_ != 0) {
        mixWord(h0, h1, pending_);
    }
    mixWord(h0, h1, length_ ^ kLengthSalt);

    h0 ^= kFinalSalt0;
    h1 ^= kFinalSalt1;
    h1 = avalanche(h1 + h0);
    h0 = avalanche(h0 + h1);

    ParamFingerprint fingerprint;
    fingerprint.words = {h0, h1};
    return fingerprint;
}

uint64_t ParamFingerprint::hash() const
{
    return avalanche(words[0] ^ (words[1] * kMixMul0));
}

std::string ParamFingerprint::hexString() const
{
    constexpr char kDigits[] = "0123456789abcdef";
    constexpr int kNibbleBits = 4;
    constexpr int kWordBits = 64;

    std::string text(WORD_COUNT * kWordBits / kNibbleBits, '0');
    size_t position = 0;
    for (const uint64_t word : words) {
        for (int shift = kWordBits - kNibbleBits; shift >= 0;
             shift -= kNibbleBits) {
            text[position++] = kDigits[(word >> shift) & UINT64_C(0xF)];
        }
    }
    assert(position == text.size());
    return text;
}

} // namespace rtengine::stage
