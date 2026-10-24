/*
 * Persistent store for stage buffers.
 *
 * A store owns one directory tree supplied by the caller (for example
 * RT_CACHE/stages). Every entry is addressed by a caller-computed key of
 * exactly 64 hexadecimal digits. The key is sharded two levels deep, so a
 * store with many entries does not put them all in one directory, and no
 * path, file name or other caller text ever reaches the file system.
 *
 * Entries are self-describing and checksummed: magic, format version, stage
 * name and version, buffer descriptor, payload length, a CRC32 of the payload
 * and a CRC32 of the header. A corrupt, truncated, foreign or mismatched file
 * is rejected instead of being returned as data. Writes go to a temporary
 * file in the destination directory, are flushed and synced, and are then
 * renamed into place, so a reader never observes a partial entry.
 *
 * Row layout: a stored row holds width * bytes_per_pixel(format) bytes per
 * plane, except a single-plane RT_STAGE_U8 buffer, which holds three bytes
 * per pixel because the engine's Image8 is packed RGB. This matches
 * stage::row_bytes(). Source and destination row and plane pitches are
 * honoured: only the packed row bytes are stored, and on restore only those
 * bytes are written, never the pitch padding.
 *
 * The store holds no engine state and is not wired into the preview pipeline.
 * All operations are serialised by one mutex because they are dominated by
 * file I/O; it is safe to call from any thread. Two instances on the same
 * root do not share that mutex, but every entry write is still atomic.
 */

#pragma once

#include "stage_abi.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace rtengine::stage
{

struct StageStoreMetadata {
    std::string stage;
    std::string version;
};

class StageStore
{
public:
    /* A key is exactly this many hexadecimal digits; letter case is ignored. */
    static constexpr size_t key_length = 64;

    /* Entry format version written and accepted by this implementation. */
    static constexpr uint32_t format_version = 1;

    explicit StageStore(std::string root);

    StageStore(const StageStore&) = delete;
    StageStore& operator=(const StageStore&) = delete;

    const std::string& root() const
    {
        return root_;
    }

    /*
     * Writes source and metadata under key. Returns false for an invalid key,
     * invalid metadata, an unusable source descriptor or any I/O failure; a
     * failed write leaves no entry behind and does not disturb an existing
     * entry with the same key.
     */
    bool put(const std::string& key, const RtStageBuffer& source,
             const StageStoreMetadata& metadata);

    /*
     * Restores an entry. Returns false and leaves destination untouched when
     * the key is unknown, the entry is corrupt or truncated, the descriptor
     * does not match destination exactly, or destination cannot hold the
     * packed rows within its own pitches. Metadata is assigned only on
     * success.
     */
    bool get(const std::string& key, RtStageBuffer& destination,
             StageStoreMetadata& metadata) const;
    bool get(const std::string& key, RtStageBuffer& destination) const;

    /* Removes one entry; true when an entry was removed. */
    bool erase(const std::string& key);

    /*
     * Evicts the oldest files until the total size is at most max_bytes.
     * Returns the number of files removed. Used by the preview cache to keep
     * the store within its configured budget.
     */
    std::size_t prune(uint64_t max_bytes);

    /* True when an entry file exists; only get() validates its contents. */
    bool contains(const std::string& key) const;

    /* Removes every file below the root and recreates the root directory. */
    void clear();

    /*
     * Total size of every regular file below the root, in bytes, for a caller
     * that enforces a size budget. A temporary file left by a crashed writer
     * counts as well, because it occupies disk space.
     */
    uint64_t bytes() const;

    static bool valid_key(const std::string& key);

private:
    /* Canonical lower-case form of an already validated key. */
    static std::string canonical_key(const std::string& key);
    std::string entry_path(const std::string& canonical) const;
    std::string temporary_name();

    mutable std::mutex mutex_;
    std::string root_;
    uint64_t instance_nonce_ = 0;
    uint64_t temp_sequence_ = 0;
};

} // namespace rtengine::stage
