#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace LivingActivity {
struct NativeAuctionOffer {uint32_t id=0,entry=0,quantity=0,copper=0,seller=0;};
struct AuctionMaterialBasket {
    uint32_t copper=0;
    std::vector<uint32_t> listings;
};
// Read-only selection from the existing maximum-sixteen-listing market view.
// No reservation or purchase authority: every native buyout still revalidates
// its listing, remaining demand, seller and the shared spending limits.
inline bool ExactAuctionMaterialBasket(std::vector<NativeAuctionOffer> offers,
    uint32_t entry,uint32_t quantity,uint32_t budget,AuctionMaterialBasket& out) {
    out={};
    if(!entry || !quantity || quantity>10000 || !budget || offers.empty() || offers.size()>16)return false;
    std::sort(offers.begin(),offers.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(size_t i=0;i<offers.size();++i) {
        const auto& offer=offers[i];
        if(!offer.id || offer.entry!=entry || !offer.quantity || !offer.copper || !offer.seller ||
            (i && offers[i-1].id==offer.id))return false;
    }
    struct Subset {uint32_t quantity=0;uint64_t copper=0;std::vector<uint32_t> ids;};
    const auto better=[](const Subset& a,const Subset& b) {
        if(a.copper!=b.copper)return a.copper<b.copper;
        if(a.ids.size()!=b.ids.size())return a.ids.size()<b.ids.size();
        return a.ids<b.ids;
    };
    const auto subsets=[&](size_t begin,size_t end) {
        std::vector<Subset> values;
        for(uint32_t mask=0;mask<(1u<<(end-begin));++mask) {
            Subset s;
            for(size_t i=begin;i<end;++i)if(mask&(1u<<(i-begin))) {
                const auto& offer=offers[i];
                // Bound before addition, including untrusted oversized stacks.
                if(offer.quantity>quantity-s.quantity || offer.copper>budget-s.copper) {
                    s.ids.clear();s.copper=uint64_t(budget)+1;break;
                }
                s.quantity+=offer.quantity;s.copper+=offer.copper;s.ids.push_back(offer.id);
            }
            if(s.copper<=budget)values.push_back(std::move(s));
        }
        return values;
    };
    // Two halves require at most 512 subsets, not a quantity-sized table or
    // exponential scan of all sixteen listings on the world thread.
    const size_t middle=offers.size()/2;
    std::map<uint32_t,Subset> right;
    for(auto s:subsets(middle,offers.size())) {
        const auto old=right.find(s.quantity);
        if(old==right.end() || better(s,old->second))right[s.quantity]=std::move(s);
    }
    bool found=false;Subset best;
    for(auto left:subsets(0,middle)) {
        const auto tail=right.find(quantity-left.quantity);
        if(tail==right.end() || left.copper+tail->second.copper>budget)continue;
        left.copper+=tail->second.copper;
        left.ids.insert(left.ids.end(),tail->second.ids.begin(),tail->second.ids.end());
        if(!found || better(left,best)){best=std::move(left);found=true;}
    }
    if(!found)return false;
    out.copper=uint32_t(best.copper);out.listings=std::move(best.ids);return true;
}
inline void PreferCompleteAuctionMaterialBasket(std::vector<NativeAuctionOffer>& offers,
    uint32_t entry,uint32_t quantity,uint32_t budget) {
    AuctionMaterialBasket basket;
    if(!ExactAuctionMaterialBasket(offers,entry,quantity,budget,basket))return;
    // Keep the existing unit-price order inside the selected set. Choosing its
    // first whole stack cannot strand the rest of an otherwise obtainable recipe.
    std::stable_partition(offers.begin(),offers.end(),[&](const auto& offer) {
        return std::binary_search(basket.listings.begin(),basket.listings.end(),offer.id);
    });
}
}
