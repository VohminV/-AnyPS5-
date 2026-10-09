#include "CpuBackend/Resources.hpp"
#include "CpuBackend/Simd.hpp"

namespace AgcDriver::CpuBackend {

std::uint64_t ContentHash(const std::uint32_t* words, std::size_t count, std::uint64_t seed) {
    return Hash64Scalar(words, count, seed);
}

bool ContentEqual(const std::uint32_t* left, const std::uint32_t* right, std::size_t count) {
    return EqualScalar(left, right, count);
}

ResourceCache::ResourceCache()
    : ResourceCache(Config{}) {
}

ResourceCache::ResourceCache(Config config)
    : config_(config) {
    if (config_.shards == 0) {
        config_.shards = 16;
    }
    if (config_.slotsPerShard == 0) {
        config_.slotsPerShard = 256;
    }
    shards_.reserve(config_.shards);
    for (std::size_t i = 0; i < config_.shards; ++i) {
        auto shard = std::make_unique<Shard>();
        shard->slots.resize(config_.slotsPerShard);
        shards_.push_back(std::move(shard));
    }
}

std::uint64_t ResourceCache::HashKey(const ResourceKey& key) const {
    std::uint64_t hash = 1469598103934665603ull;
    hash ^= key.address + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    hash ^= key.sizeBytes + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    std::uint64_t packed = (static_cast<std::uint64_t>(key.format) << 32ull) | key.layout;
    hash ^= packed + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    hash ^= key.version + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    hash ^= key.epoch + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    hash ^= key.writeSerial + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    std::uint64_t kindPacked = (static_cast<std::uint64_t>(key.kind) << 32ull) | key.extra;
    hash ^= kindPacked + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
    hash *= 1099511628211ull;
    hash ^= hash >> 29ull;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 32ull;
    return hash;
}

std::uint32_t ResourceCache::ShardFor(std::uint64_t keyHash) const {
    return static_cast<std::uint32_t>(keyHash % static_cast<std::uint64_t>(shards_.size()));
}

std::uint32_t ResourceCache::SlotFor(std::uint64_t keyHash, std::size_t slots) const {
    return static_cast<std::uint32_t>((keyHash >> 16ull) % static_cast<std::uint64_t>(slots));
}

LookupOutcome ResourceCache::Lookup(const ResourceKey& key, const std::uint32_t* content, std::size_t words) {
    LookupOutcome out{};
    std::uint64_t keyHash = HashKey(key);
    std::uint32_t shardIndex = ShardFor(keyHash);
    Shard& shard = *shards_[shardIndex];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.uses += 1;
    std::uint32_t start = SlotFor(keyHash, shard.slots.size());
    std::uint64_t contentHash = 0;
    bool hashed = false;
    for (std::size_t probe = 0; probe < shard.slots.size(); ++probe) {
        std::uint32_t index = static_cast<std::uint32_t>((static_cast<std::size_t>(start) + probe) % shard.slots.size());
        Slot& slot = shard.slots[index];
        if (!slot.occupied) {
            break;
        }
        if (slot.keyHash != keyHash) {
            continue;
        }
        if (!(slot.key == key)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> statLock(statsMutex_);
            stats_.fullCompares += 1;
        }
        std::size_t stored = slot.overflow.empty() ? slot.inlineCount : slot.overflow.size();
        if (stored != words) {
            continue;
        }
        if (!hashed) {
            if (content != nullptr && words != 0) {
                contentHash = Hash64Scalar(content, words, keyHash);
            } else {
                contentHash = keyHash ^ 0x9e3779b97f4a7c15ull;
            }
            hashed = true;
        }
        if (slot.contentHash != contentHash) {
            continue;
        }
        if (words != 0) {
            ::AgcDriver::CpuBackend::Path active = static_cast<::AgcDriver::CpuBackend::Path>(path_);
            const std::uint32_t* storedData = slot.overflow.empty() ? slot.inlineContent.data() : slot.overflow.data();
            if (!EqualDispatch(storedData, content, words, active)) {
                continue;
            }
        }
        if (!slot.live) {
            continue;
        }
        slot.lastUse = shard.uses;
        out.hit = true;
        out.contentHash = slot.contentHash;
        out.shard = shardIndex;
        out.slot = index;
        {
            std::lock_guard<std::mutex> statLock(statsMutex_);
            stats_.hits += 1;
        }
        return out;
    }
    {
        std::lock_guard<std::mutex> statLock(statsMutex_);
        stats_.misses += 1;
    }
    out.hit = false;
    out.shard = shardIndex;
    out.slot = start;
    return out;
}

bool ResourceCache::Insert(const ResourceKey& key, const std::uint32_t* content, std::size_t words, std::uint64_t contentHash) {
    std::uint64_t keyHash = HashKey(key);
    std::uint32_t shardIndex = ShardFor(keyHash);
    Shard& shard = *shards_[shardIndex];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.uses += 1;
    std::uint64_t realHash = keyHash ^ 0x9e3779b97f4a7c15ull;
    if (content != nullptr && words != 0) {
        realHash = Hash64Scalar(content, words, keyHash);
    }
    (void)contentHash;
    std::uint32_t start = SlotFor(keyHash, shard.slots.size());
    std::uint32_t empty = 0xFFFFFFFFu;
    std::uint32_t oldest = start;
    std::uint64_t oldestUse = shard.slots[start].lastUse;
    bool oldestSet = false;
    for (std::size_t probe = 0; probe < shard.slots.size(); ++probe) {
        std::uint32_t index = static_cast<std::uint32_t>((static_cast<std::size_t>(start) + probe) % shard.slots.size());
        Slot& slot = shard.slots[index];
        if (!slot.occupied) {
            empty = index;
            break;
        }
        if (slot.keyHash == keyHash && slot.key == key) {
            slot.contentHash = realHash;
            if (words <= 16) {
                for (std::size_t i = 0; i < words; ++i) {
                    slot.inlineContent[i] = content[i];
                }
                slot.inlineCount = words;
                slot.overflow.clear();
            } else {
                slot.inlineCount = 0;
                slot.overflow.assign(content, content + words);
            }
            slot.live = true;
            slot.lastUse = shard.uses;
            slot.bytes = static_cast<std::uint64_t>(words) * 4ull;
            {
                std::lock_guard<std::mutex> statLock(statsMutex_);
                stats_.inserts += 1;
            }
            return true;
        }
        if (!oldestSet || slot.lastUse < oldestUse) {
            oldestUse = slot.lastUse;
            oldest = index;
            oldestSet = true;
        }
    }
    std::uint32_t target = (empty != 0xFFFFFFFFu) ? empty : oldest;
    bool evict = (empty == 0xFFFFFFFFu);
    Slot& slot = shard.slots[target];
    slot.occupied = true;
    slot.live = true;
    slot.key = key;
    slot.keyHash = keyHash;
    slot.contentHash = realHash;
    if (words <= 16) {
        for (std::size_t i = 0; i < words; ++i) {
            slot.inlineContent[i] = content[i];
        }
        slot.inlineCount = words;
        slot.overflow.clear();
    } else {
        slot.inlineCount = 0;
        slot.overflow.assign(content, content + words);
    }
    slot.lastUse = shard.uses;
    slot.bytes = static_cast<std::uint64_t>(words) * 4ull;
    {
        std::lock_guard<std::mutex> statLock(statsMutex_);
        stats_.inserts += 1;
        if (evict) {
            stats_.evictions += 1;
        }
    }
    return true;
}

std::size_t ResourceCache::InvalidateRange(std::uint64_t address, std::uint64_t sizeBytes) {
    std::size_t removed = 0;
    std::uint64_t end = address + sizeBytes;
    for (auto& entry : shards_) {
        Shard& shard = *entry;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto& slot : shard.slots) {
            if (!slot.occupied || !slot.live) {
                continue;
            }
            std::uint64_t slotEnd = slot.key.address + slot.key.sizeBytes;
            bool overlap = !(slotEnd <= address || slot.key.address >= end);
            if (overlap) {
                slot.live = false;
                removed += 1;
            }
        }
    }
    {
        std::lock_guard<std::mutex> statLock(statsMutex_);
        stats_.invalidations += removed;
    }
    return removed;
}

void ResourceCache::Clear() {
    for (auto& entry : shards_) {
        Shard& shard = *entry;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto& slot : shard.slots) {
            slot.occupied = false;
            slot.live = false;
            slot.overflow.clear();
            slot.inlineCount = 0;
            slot.contentHash = 0;
            slot.keyHash = 0;
            slot.lastUse = 0;
            slot.bytes = 0;
        }
    }
}

ResourceStats ResourceCache::Stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

std::size_t ResourceCache::LiveEntries() const {
    std::size_t total = 0;
    for (auto& entry : shards_) {
        Shard& shard = *entry;
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shard.mutex));
        for (auto& slot : shard.slots) {
            if (slot.occupied && slot.live) {
                total += 1;
            }
        }
    }
    return total;
}

std::size_t ResourceCache::LiveBytes() const {
    std::size_t total = 0;
    for (auto& entry : shards_) {
        Shard& shard = *entry;
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(shard.mutex));
        for (auto& slot : shard.slots) {
            if (slot.occupied && slot.live) {
                total += slot.bytes;
            }
        }
    }
    return total;
}

void ResourceCache::SetPath(std::uint8_t path) {
    path_ = path;
}

std::uint8_t ResourceCache::Path() const {
    return path_;
}

} // namespace AgcDriver::CpuBackend
