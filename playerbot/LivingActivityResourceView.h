#ifndef LIVING_ACTIVITY_RESOURCE_VIEW_H
#define LIVING_ACTIVITY_RESOURCE_VIEW_H
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace LivingActivity {
    using HeldBagItemKey = std::tuple<std::string,uint32_t,uint32_t,uint32_t>; // root, actor, GUID, entry
    struct ResourceClaim;
    struct ResourceProtection;
    // Immutable, partitioned index: a transition copies only its affected
    // buckets, not every bot's claims. No native pointers or execution grants.
    struct ResourceBucket {
        std::map<uint32_t,uint64_t> items, money;
        std::map<std::pair<uint32_t,uint32_t>,uint64_t> uncertain;
        std::map<HeldBagItemKey,uint64_t> heldBagItems;
    };
    struct ResourceView {
        bool ready = false;
        uint64_t revision = 0;
        std::array<std::shared_ptr<const ResourceBucket>,256> buckets{};
        uint32_t UnreservedItem(uint32_t actor,uint32_t guid,uint32_t entry,uint32_t nativeCount) const;
        // Exact immutable protection, not the clamped available quantity. A
        // service must not mistake an overclaimed full stack for its own claim.
        uint64_t ProtectedItem(uint32_t guid) const;
        // Acknowledged held bag portions only. Pending, banked or uncertain
        // quantities remain protected globally but are never usable backing.
        uint64_t HeldBagItem(const std::string& root,uint32_t actor,uint32_t guid,uint32_t entry) const;
        bool HasUncertainItem(uint32_t actor,uint32_t entry) const;
        uint32_t UnreservedMoney(uint32_t actor,uint32_t nativeCopper) const;
    };
    class ResourcePublisher;
    class ResourceReader {
    public:
        ResourceReader() = default;
        std::shared_ptr<const ResourceView> Inspect() const;
    private:
        friend class ResourcePublisher;
        struct Cell { std::shared_ptr<const ResourceView> value; };
        explicit ResourceReader(std::shared_ptr<Cell> cell) : cell(std::move(cell)) {}
        std::shared_ptr<Cell> cell;
    };
    // World-owned only. Reader handles cannot publish or release protection.
    class ResourcePublisher {
    public:
        ResourcePublisher() : cell(std::make_shared<ResourceReader::Cell>()) {}
        ResourcePublisher(const ResourcePublisher&) = delete;
        ResourcePublisher& operator=(const ResourcePublisher&) = delete;
        ResourceReader Reader() const { return ResourceReader(cell); }
        void Changed(const ResourceClaim& claim);
        void Publish(const ResourceProtection& source);
    private:
        std::shared_ptr<ResourceReader::Cell> cell;
        std::set<uint32_t> items, money;
        std::set<std::pair<uint32_t,uint32_t>> uncertain;
        std::set<HeldBagItemKey> heldBagItems;
    };
}
#endif
