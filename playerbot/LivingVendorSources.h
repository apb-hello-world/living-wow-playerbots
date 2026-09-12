#ifndef LIVING_VENDOR_SOURCES_H
#define LIVING_VENDOR_SOURCES_H
#include <cstdint>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace LivingActivity {
    // Built once with native travel destinations, including limited stock.
    // These are seller candidates, never stock reservations or purchase proof.
    // A partial/overflowed catalogue must not silently look complete.
    class VendorSourceIndex {
    public:
        explicit VendorSourceIndex(std::size_t limit=250000) : limit(limit) {}
        void Clear() { sources.clear();count=0;sealed=false;overflow=false; }
        void Add(uint32_t item,uint32_t vendor) {
            if(sealed || overflow || !item || !vendor) return;
            auto found=sources.find(item);
            if(found!=sources.end() && found->second.count(vendor)) return;
            if(count>=limit) {overflow=true;return;}
            sources[item].insert(vendor);++count;
        }
        void Seal() {sealed=true;}
        bool Ready() const {return sealed && !overflow;}
        const std::set<uint32_t>& Sellers(uint32_t item) const {
            static const std::set<uint32_t> empty;
            const auto found=sources.find(item);
            return Ready() && found!=sources.end()?found->second:empty;
        }
    private:
        std::map<uint32_t,std::set<uint32_t>> sources;
        std::size_t limit,count=0;
        bool sealed=false,overflow=false;
    };
}
#endif
