#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::CpuBackend {

struct ResourceKey {
    std::uint64_t address = 0;
    std::uint64_t sizeBytes = 0;
    std::uint32_t format = 0;
    std::uint32_t layout = 0;
    std::uint64_t version = 0;
    std::uint64_t epoch = 0;
    std::uint64_t writeSerial = 0;
    std::uint32_t kind = 0;
    std::uint32_t extra = 0;
    bool operator==(const ResourceKey& other) const {
        return address == other.address && sizeBytes == other.sizeBytes && format == other.format &&
               layout == other.layout && version == other.version && epoch == other.epoch &&
               writeSerial == other.writeSerial && kind == other.kind && extra == other.extra;
    }
};

struct ResourceStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t inserts = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t fullCompares = 0;
};

struct LookupOutcome {
    bool hit = false;
    std::uint64_t contentHash = 0;
    std::uint32_t shard = 0;
    std::uint32_t slot = 0;
};

class ResourceCache {
public:
    struct Config {
        std::size_t shards = 16;
        std::size_t slotsPerShard = 256;
        std::size_t maxBytes = 256u * 1024u * 1024u;
    };
    explicit ResourceCache();
    explicit ResourceCache(Config config);
    ResourceCache(const ResourceCache&) = delete;
    ResourceCache& operator=(const ResourceCache&) = delete;
    std::uint64_t HashKey(const ResourceKey& key) const;
    LookupOutcome Lookup(const ResourceKey& key, const std::uint32_t* content, std::size_t words);
    bool Insert(const ResourceKey& key, const std::uint32_t* content, std::size_t words, std::uint64_t contentHash);
    std::size_t InvalidateRange(std::uint64_t address, std::uint64_t sizeBytes);
    void Clear();
    ResourceStats Stats() const;
    std::size_t LiveEntries() const;
    std::size_t LiveBytes() const;
    void SetPath(std::uint8_t path);
    std::uint8_t Path() const;

private:
    struct Slot {
        bool occupied = false;
        bool live = false;
        ResourceKey key{};
        std::uint64_t keyHash = 0;
        std::uint64_t contentHash = 0;
        std::array<std::uint32_t, 16> inlineContent{};
        std::size_t inlineCount = 0;
        std::vector<std::uint32_t> overflow;
        std::uint64_t lastUse = 0;
        std::uint64_t bytes = 0;
    };
    struct Shard {
        std::mutex mutex;
        std::vector<Slot> slots;
        std::uint64_t uses = 0;
    };
    std::uint32_t ShardFor(std::uint64_t keyHash) const;
    std::uint32_t SlotFor(std::uint64_t keyHash, std::size_t slots) const;
    std::vector<std::unique_ptr<Shard>> shards_;
    Config config_;
    mutable std::mutex statsMutex_;
    ResourceStats stats_;
    std::uint8_t path_ = 0;
};

std::uint64_t ContentHash(const std::uint32_t* words, std::size_t count, std::uint64_t seed);
bool ContentEqual(const std::uint32_t* left, const std::uint32_t* right, std::size_t count);

} // namespace AgcDriver::CpuBackend
