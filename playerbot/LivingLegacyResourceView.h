#ifndef LIVING_LEGACY_RESOURCE_VIEW_H
#define LIVING_LEGACY_RESOURCE_VIEW_H
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace LivingActivity {
    // Transitional protection only, not a task/claim or an execution grant.
    // Old producers publish when reservations change; map workers retain a
    // value-only snapshot and never traverse their mutable manager containers.
    struct LegacyResourceView {
        std::set<uint32_t> items;
        std::set<std::pair<uint32_t,uint32_t>> entries;
        bool Item(uint32_t guid) const { return guid && items.count(guid); }
        bool Entry(uint32_t actor,uint32_t entry) const { return actor && entry && entries.count({actor,entry}); }
    };
    class LegacyResourcePublisher {
    public:
        LegacyResourcePublisher() : value(std::make_shared<const LegacyResourceView>()) {}
        std::shared_ptr<const LegacyResourceView> Inspect() const {
            return std::atomic_load_explicit(&value,std::memory_order_acquire);
        }
        void SetItem(uint32_t guid,bool held) {
            if (!guid) return;
            std::lock_guard<std::mutex> lock(writer);
            const auto old=Inspect();
            if (old->Item(guid)==held) return;
            auto next=std::make_shared<LegacyResourceView>(*old);
            if (held) next->items.insert(guid); else next->items.erase(guid);
            Store(std::move(next));
        }
        void Replace(LegacyResourceView next) {
            std::lock_guard<std::mutex> lock(writer);
            const auto old=Inspect();
            if (old->items==next.items && old->entries==next.entries) return;
            Store(std::make_shared<LegacyResourceView>(std::move(next)));
        }
    private:
        void Store(std::shared_ptr<const LegacyResourceView> next) {
            std::atomic_store_explicit(&value,std::move(next),std::memory_order_release);
        }
        std::mutex writer;
        std::shared_ptr<const LegacyResourceView> value;
    };
}
#endif
