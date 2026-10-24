#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <glibmm/ustring.h>

namespace rtengine::stage
{

namespace detail
{

/*
 * Framing tags for the fingerprint byte stream. These values are part of the
 * persisted format: change one and every stored fingerprint becomes invalid.
 * Zero is unused, so a zeroed buffer never reads as a tagged field.
 */
enum class ParamTag : uint8_t {
    kSection = 0x01,
    kString = 0x02,
    kBool = 0x03,
    kInt32 = 0x04,
    kInt64 = 0x05,
    kUInt32 = 0x06,
    kUInt64 = 0x07,
    kFloat = 0x08,
    kDouble = 0x09,
};

} // namespace detail

/*
 * Stable 128-bit fingerprint of a parameter set.
 *
 * The two words are opaque to callers: compare them, hash them or print them,
 * but never interpret them. A fingerprint is published in stage cache keys and
 * profiling output, so the value must stay identical across processes, builds,
 * compilers and platforms. It is deliberately not cryptographic, and it makes
 * no collision-resistance claim beyond the birthday bound of 128 bits.
 */
struct ParamFingerprint {
    static constexpr size_t WORD_COUNT = 2;

    std::array<uint64_t, WORD_COUNT> words{};

    bool operator==(const ParamFingerprint& other) const
    {
        return words == other.words;
    }

    bool operator!=(const ParamFingerprint& other) const
    {
        return words != other.words;
    }

    /* Mixed 64-bit value for unordered containers. Not a prefix of `words`. */
    uint64_t hash() const;

    /* Lower-case hexadecimal form, 32 characters, for logs and tests. */
    std::string hexString() const;
};

/*
 * Streaming parameter hasher.
 *
 * Each add() feeds one typed value. The type tag, the value bytes and the
 * earlier bytes all affect the result, so:
 *
 * - order matters: add(1); add(2) differs from add(2); add(1);
 * - type matters: int32_t(1) differs from int64_t(1) and from double(1.0);
 * - lengths matter: "abc" differs from "abcd", and two strings cannot be
 *   retyped into one another by moving a character across the boundary.
 *
 * Every numeric value is hashed by its bit pattern in a fixed little-endian
 * layout, never by its decimal formatting, so a value cannot collide with a
 * string and platform formatting cannot change the fingerprint. Float and
 * double payloads, including the sign of zero and NaN payload bits, are
 * preserved exactly; callers that want -0.0 to equal 0.0 must normalise the
 * value before adding it.
 *
 * addSectionLabel() provides domain separation: label a stage or a section
 * before adding its fields, so the same field values in two sections produce
 * different fingerprints. Use one label per stage descriptor, and never
 * concatenate labels into a single string.
 *
 * finish() does not modify the hasher, so the same instance can be finished
 * and then extended.
 *
 * A hasher is not thread-safe. Give each thread its own instance, or build the
 * descriptor once and share the resulting fingerprint.
 */
class ParamHasher
{
public:
    ParamHasher();

    /*
     * Byte length of a fingerprint in hexadecimal form: one character per
     * nibble of each 64-bit word. Used for fixed-size log buffers.
     */
    static constexpr size_t HEX_LENGTH = ParamFingerprint::WORD_COUNT * 16;

    /*
     * Start a new labelled section. The label is length-prefixed and tagged
     * separately from addString(), so a label never aliases a string field.
     */
    void addSectionLabel(const std::string& label);
    void addSectionLabel(const char* label);

    /* Scalar types. Each one carries a distinct type tag. */
    void add(bool value);
    void add(int32_t value);
    void add(int64_t value);
    void add(uint32_t value);
    void add(uint64_t value);
    void add(float value);
    void add(double value);

    /*
     * A string literal would otherwise reach add(bool) through pointer
     * conversion. Reject it at compile time instead of hashing a constant.
     */
    void add(const char* value) = delete;

    void addString(const std::string& value);
    void addString(const Glib::ustring& value);
    /* Explicit overload so a literal is not ambiguous between the two above. */
    void addString(const char* value);

    /*
     * Raw bytes, for arrays and packed structures. The byte stream is
     * length-framed as a whole, so a trailing zero byte is significant. The
     * caller must pass the same byte order on every platform; for portable
     * values use the typed overloads.
     */
    void addBytes(const void* data, size_t size);

    ParamFingerprint finish() const;

private:
    void addTagged(detail::ParamTag tag, const void* data, size_t size);
    void addStringBytes(detail::ParamTag tag, const void* data, size_t size);

    uint64_t state0_;
    uint64_t state1_;
    uint64_t length_ = 0;
    uint64_t pending_ = 0;
    uint32_t pending_bytes_ = 0;
};

} // namespace rtengine::stage
