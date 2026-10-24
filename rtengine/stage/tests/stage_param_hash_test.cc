#include "stage_param_hash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

template <typename T>
rtengine::stage::ParamFingerprint single(T value)
{
    rtengine::stage::ParamHasher hasher;
    hasher.add(value);
    return hasher.finish();
}

rtengine::stage::ParamFingerprint stringFingerprint(const std::string& value)
{
    rtengine::stage::ParamHasher hasher;
    hasher.addString(value);
    return hasher.finish();
}

/* One labelled section holding one integer, the shape of a stage descriptor. */
rtengine::stage::ParamFingerprint labelled(const char* label, int32_t value)
{
    rtengine::stage::ParamHasher hasher;
    hasher.addSectionLabel(label);
    hasher.add(value);
    return hasher.finish();
}

} // namespace

int main()
{
    using rtengine::stage::ParamFingerprint;
    using rtengine::stage::ParamHasher;

    /*
     * A representative descriptor: three labelled sections with an integer, a
     * double and a string field.
     */
    const auto sample = [](int32_t temperature, double exposure,
                           const std::string& curve) {
        ParamHasher hasher;
        hasher.addSectionLabel("white-balance");
        hasher.add(temperature);
        hasher.addSectionLabel("exposure");
        hasher.add(exposure);
        hasher.addSectionLabel("tone-curve");
        hasher.addString(curve);
        return hasher.finish();
    };

    /* Criterion 1: identical inputs produce identical fingerprints. */
    check(sample(6500, 0.5, "film") == sample(6500, 0.5, "film"),
          "identical descriptors produce equal fingerprints");
    check(sample(6500, 0.5, "film").hexString() ==
              sample(6500, 0.5, "film").hexString(),
          "identical descriptors produce the same hexadecimal form");
    check(sample(6500, 0.5, "film").hash() ==
              sample(6500, 0.5, "film").hash(),
          "identical descriptors produce the same container hash");

    /*
     * The format is persisted in cache keys: this published value guards it
     * against an accidental change to the constants, tags or framing.
     */
    {
        ParamHasher golden;
        golden.addSectionLabel("golden");
        golden.add(int32_t{6500});
        golden.add(0.5);
        golden.addString("film");
        check(golden.finish().hexString() ==
                  "553f1f9e677666f509db98fafda02b42",
              "the published fingerprint value is stable");
    }

    /* The three string overloads describe the same byte stream. */
    {
        ParamHasher literal;
        literal.addString("film");
        ParamHasher owned;
        owned.addString(std::string("film"));
        ParamHasher unicode;
        unicode.addString(Glib::ustring("film"));
        check(literal.finish() == owned.finish(),
              "string literal and std::string agree");
        check(owned.finish() == unicode.finish(),
              "std::string and Glib::ustring agree");
    }

    /* Criterion 2: changing any single field changes the fingerprint. */
    check(sample(6501, 0.5, "film") != sample(6500, 0.5, "film"),
          "an integer field change alters the fingerprint");
    check(sample(6500, 0.5 + 1.0 / 1024.0, "film") !=
              sample(6500, 0.5, "film"),
          "a double field change alters the fingerprint");
    check(sample(6500, 0.5, "filn") != sample(6500, 0.5, "film"),
          "a string field change alters the fingerprint");
    check(sample(6500, 0.5, "film") != sample(-6500, 0.5, "film"),
          "a sign change alters the fingerprint");

    /* Criterion 3a: field order matters. */
    ParamHasher ascending;
    ascending.add(int32_t{1});
    ascending.add(int32_t{2});
    ParamHasher descending;
    descending.add(int32_t{2});
    descending.add(int32_t{1});
    check(ascending.finish() != descending.finish(),
          "reordered fields produce a different fingerprint");

    /* Criterion 3b: the declared type matters. */
    const ParamFingerprint as_int32 = single(int32_t{1});
    const ParamFingerprint as_int64 = single(int64_t{1});
    const ParamFingerprint as_uint32 = single(uint32_t{1});
    const ParamFingerprint as_uint64 = single(uint64_t{1});
    const ParamFingerprint as_bool = single(true);
    const ParamFingerprint as_float = single(1.0f);
    const ParamFingerprint as_double = single(1.0);
    check(as_int32 != as_int64, "int32 and int64 of one differ");
    check(as_int32 != as_uint32, "int32 and uint32 of one differ");
    check(as_int64 != as_uint64, "int64 and uint64 of one differ");
    check(as_bool != as_int32, "bool true and int32 one differ");
    check(as_float != as_double, "float and double of one differ");
    check(as_float != as_int32, "float and int32 of one differ");
    check(as_int32 != stringFingerprint("1"),
          "an integer and its decimal string differ");

    /* Criterion 4: strings with shared prefixes differ. */
    check(stringFingerprint("abc") != stringFingerprint("abcd"),
          "a string and its prefix differ");
    check(stringFingerprint("abc") != stringFingerprint("abd"),
          "strings differing in the last byte differ");
    check(stringFingerprint(std::string("abc", 3)) !=
              stringFingerprint(std::string("abc\0", 4)),
          "a trailing zero byte is significant");

    ParamHasher split_left;
    split_left.addString("ab");
    split_left.addString("c");
    ParamHasher split_right;
    split_right.addString("a");
    split_right.addString("bc");
    check(split_left.finish() != split_right.finish(),
          "moving a character across a string boundary differs");

    ParamHasher empty_field;
    empty_field.addString(std::string());
    check(empty_field.finish() != ParamHasher().finish(),
          "an empty string differs from an absent field");

    /* Criterion 5: floats and doubles hash by bit pattern, not by value. */
    check(single(1.0f) != single(std::nextafter(1.0f, 2.0f)),
          "adjacent floats produce different fingerprints");
    check(single(1.0) != single(std::nextafter(1.0, 2.0)),
          "adjacent doubles produce different fingerprints");
    check(single(0.0f) != single(-0.0f),
          "positive and negative zero differ as float bit patterns");
    check(single(0.0) != single(-0.0),
          "positive and negative zero differ as double bit patterns");
    check(single(0.0f) != single(0.0),
          "float zero and double zero differ");

    /* Criterion 6: the same value in different labelled sections differs. */
    check(labelled("white-balance", 1) != labelled("tone-curve", 1),
          "the same field value in two sections differs");
    check(labelled("wb", 1) != labelled("wb2", 1),
          "labels with a shared prefix differ");
    check(labelled("wb", 1) != labelled("wb", 2),
          "a field change inside one section differs");
    {
        ParamHasher from_literal;
        from_literal.addSectionLabel("wb");
        ParamHasher from_string;
        from_string.addSectionLabel(std::string("wb"));
        check(from_literal.finish() == from_string.finish(),
              "label overloads agree");
    }
    {
        ParamHasher as_label;
        as_label.addSectionLabel("curve");
        check(as_label.finish() != stringFingerprint("curve"),
              "a section label and a string of the same text differ");
    }

    /* Criterion 7: raw byte framing. */
    {
        const unsigned char one[1] = {0x01};
        const unsigned char two[2] = {0x01, 0x00};
        ParamHasher short_bytes;
        short_bytes.addBytes(one, sizeof(one));
        ParamHasher long_bytes;
        long_bytes.addBytes(two, sizeof(two));
        check(short_bytes.finish() != long_bytes.finish(),
              "raw byte arrays of different lengths differ");

        const unsigned char word[4] = {0x01, 0x00, 0x00, 0x00};
        ParamHasher split;
        split.addBytes(word, 1);
        split.addBytes(word + 1, 3);
        ParamHasher whole;
        whole.addBytes(word, sizeof(word));
        check(split.finish() == whole.finish(),
              "a split byte stream matches the unsplit stream");

        ParamHasher typed;
        typed.add(uint32_t{1});
        check(whole.finish() != typed.finish(),
              "raw bytes and a typed value differ");
    }

    /* finish() is a pure operation: it can be called again and extended. */
    {
        ParamHasher hasher;
        hasher.addSectionLabel("exposure");
        hasher.add(0.25);
        const ParamFingerprint before = hasher.finish();
        check(before == hasher.finish(), "finish is repeatable");

        hasher.add(int32_t{7});
        check(before != hasher.finish(), "extending changes the fingerprint");

        ParamHasher replay;
        replay.addSectionLabel("exposure");
        replay.add(0.25);
        check(before == replay.finish(),
              "finish does not consume hasher state");
    }

    /* Criterion 8: 100k repeated calls are deterministic. */
    {
        const ParamFingerprint expected = sample(6500, 0.5, "film");
        const std::string expected_hex = expected.hexString();
        constexpr int kRepetitions = 100000;
        bool stable = true;
        for (int i = 0; i < kRepetitions; ++i) {
            const ParamFingerprint current = sample(6500, 0.5, "film");
            if (current != expected || current.hash() != expected.hash() ||
                current.hexString() != expected_hex) {
                stable = false;
                break;
            }
        }
        check(stable, "100k repeated fingerprints are identical");
        check(expected.hexString() == expected_hex,
              "100k repeated hexadecimal forms are identical");
    }

    if (failures == 0) {
        std::puts("stage-param-hash-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
