#include "LivingActivityResourceView.h"
#include "LivingActivityResources.h"
#include "LivingActivityEffects.h"
#include <atomic>
#include <limits>

namespace LivingActivity {
    namespace {
        template<class Map,class Key> uint64_t Get(const Map& map,const Key& key) {
            const auto found=map.find(key); return found == map.end() ? 0 : found->second;
        }
        template<class Map,class Key> void Set(Map& map,const Key& key,uint64_t value) {
            if (value) map[key]=value; else map.erase(key);
        }
        uint32_t Available(uint32_t native,uint64_t claimed) { return claimed >= native ? 0 : native-uint32_t(claimed); }
    }
    uint32_t ResourceView::UnreservedItem(uint32_t actor,uint32_t guid,uint32_t entry,uint32_t nativeCount) const {
        if (!ready || !actor || !guid || !entry) return 0;
        const auto& item=buckets[guid%256]; const auto& owner=buckets[actor%256];
        const uint64_t known=item ? Get(item->items,guid) : 0;
        const uint64_t unknown=owner ? Get(owner->uncertain,std::make_pair(actor,entry)) : 0;
        if (known > std::numeric_limits<uint64_t>::max()-unknown) return 0;
        return Available(nativeCount,known+unknown);
    }
    uint32_t ResourceView::UnreservedMoney(uint32_t actor,uint32_t nativeCopper) const {
        if (!ready || !actor) return 0;
        const auto& owner=buckets[actor%256];
        return Available(nativeCopper,owner ? Get(owner->money,actor) : 0);
    }
    uint32_t ResourceView::NativeBlockedEffects(uint32_t actor) const {
        if (!ready || !actor) return AllEffects;
        const auto& owner=buckets[actor%256];
        if (!owner || !Get(owner->actorClaims,actor)) return 0;
        // Travel and factual social output do not consume reserved goods.
        // Spell/equipment/guild operations can consume or transfer them, even
        // if a coarse action declaration omitted Inventory or Money.
        return Mask(Effect::Inventory) | Mask(Effect::Money) | Mask(Effect::Spell) |
            Mask(Effect::Equipment) | Mask(Effect::Guild);
    }
    uint64_t ResourceView::ProtectedItem(uint32_t guid) const {
        if (!ready || !guid) return std::numeric_limits<uint64_t>::max();
        const auto& bucket=buckets[guid%256];return bucket ? Get(bucket->items,guid) : 0;
    }
    uint64_t ResourceView::HeldBagItem(const std::string& root,uint32_t actor,uint32_t guid,uint32_t entry) const {
        if(!ready || root.empty() || !actor || !guid || !entry) return 0;
        const auto& bucket=buckets[guid%256];
        return bucket ? Get(bucket->heldBagItems,HeldBagItemKey{root,actor,guid,entry}) : 0;
    }
    bool ResourceView::HasUncertainItem(uint32_t actor,uint32_t entry) const {
        if (!ready || !actor || !entry) return true;
        const auto& bucket=buckets[actor%256];return bucket && Get(bucket->uncertain,std::make_pair(actor,entry));
    }
    std::shared_ptr<const ResourceView> ResourceReader::Inspect() const {
        if (!cell) return {};
        return std::atomic_load_explicit(&cell->value,std::memory_order_acquire);
    }
    void ResourcePublisher::Changed(const ResourceClaim& claim) {
        actors.insert(claim.actor);
        if (claim.copper) { if (claim.location == "money") money.insert(claim.actor); }
        else if (claim.itemGuid) {
            items.insert(claim.itemGuid);
            heldBagItems.emplace(claim.task,claim.actor,claim.itemGuid,claim.itemEntry);
        }
        else uncertain.emplace(claim.actor,claim.itemEntry);
    }
    void ResourcePublisher::Publish(const ResourceProtection& source) {
        auto previous=std::atomic_load_explicit(&cell->value,std::memory_order_acquire);
        auto next=previous ? std::make_shared<ResourceView>(*previous) : std::make_shared<ResourceView>();
        std::map<size_t,std::shared_ptr<ResourceBucket>> changed;
        auto bucket=[&](uint32_t id)->ResourceBucket& {
            const size_t index=id%256;
            auto found=changed.find(index);
            if (found == changed.end()) found=changed.emplace(index,next->buckets[index] ?
                std::make_shared<ResourceBucket>(*next->buckets[index]) : std::make_shared<ResourceBucket>()).first;
            return *found->second;
        };
        for (const auto guid : items) Set(bucket(guid).items,guid,Get(source.items,guid));
        for (const auto actor : money) Set(bucket(actor).money,actor,Get(source.money,actor));
        for (const auto actor : actors) Set(bucket(actor).actorClaims,actor,Get(source.actorClaims,actor));
        for (const auto& key : uncertain) Set(bucket(key.first).uncertain,key,Get(source.uncertainEntries,key));
        for (const auto& key : heldBagItems) Set(bucket(std::get<2>(key)).heldBagItems,key,Get(source.heldBagItems,key));
        for (const auto& row : changed) next->buckets[row.first]=row.second;
        next->ready=source.ready; next->revision=source.revision;
        std::shared_ptr<const ResourceView> immutable=next;
        std::atomic_store_explicit(&cell->value,std::move(immutable),std::memory_order_release);
        items.clear(); money.clear(); actors.clear(); uncertain.clear(); heldBagItems.clear();
    }
}
