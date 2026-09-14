#pragma once
#include "LivingAuctionMaterialSource.h"
#include <cassert>
#include <random>

inline void TestAuctionMaterialSource() {
    using namespace LivingActivity;
    AuctionMaterialBasket result;
    // Previously the guild candidate required one listing to cover everything.
    std::vector<NativeAuctionOffer> offers{{21,765,2,3,8},{22,765,2,3,9}};
    assert(ExactAuctionMaterialBasket(offers,765,4,6,result));
    assert(result.copper==6 && (result.listings==std::vector<uint32_t>{21,22}));
    assert(!ExactAuctionMaterialBasket(offers,765,4,5,result) && result.listings.empty());
    assert(!ExactAuctionMaterialBasket(offers,765,5,100,result));
    // A cheap three-unit stack would strand the last reagent. The real buyout
    // executor uses the same ordering, avoiding that greedy dead end.
    offers.insert(offers.begin(),{20,765,3,1,10});
    offers.push_back({23,765,4,7,11});
    PreferCompleteAuctionMaterialBasket(offers,765,4,20);
    assert(offers[0].id==21 && offers[1].id==22 && offers[2].id==20);
    assert(ExactAuctionMaterialBasket(offers,765,4,20,result) && result.copper==6);
    // Sequential native purchases collect mail, then re-read remaining demand.
    uint32_t remaining=4,wallet=20,paid=0;
    while(remaining) {
        PreferCompleteAuctionMaterialBasket(offers,765,remaining,wallet);
        const auto bought=offers.front();
        assert(bought.quantity<=remaining && bought.copper<=wallet);
        remaining-=bought.quantity;wallet-=bought.copper;paid+=bought.copper;
        offers.erase(offers.begin()); // No reuse of a sold listing.
        offers.erase(std::remove_if(offers.begin(),offers.end(),[&](const auto& o) {
            return o.quantity>remaining || o.copper>wallet;
        }),offers.end());
    }
    assert(paid==6);
    // Changed stock is not a promise or a synthetic order. Partial execution
    // remains possible, but candidate admission cannot claim complete sourcing.
    offers={{21,765,2,3,8}};
    assert(!ExactAuctionMaterialBasket(offers,765,4,100,result));
    PreferCompleteAuctionMaterialBasket(offers,765,4,100);
    assert(offers.front().id==21);
    // Equal cost prefers fewer purchases, then stable native listing IDs.
    offers={{8,765,2,4,8},{7,765,2,4,8},{2,765,1,2,8},{3,765,1,2,8}};
    assert(ExactAuctionMaterialBasket(offers,765,2,4,result));
    assert((result.listings==std::vector<uint32_t>{7}));
    offers.clear();
    for(uint32_t n=1;n<=16;++n)offers.push_back({n,765,1,1,8});
    assert(ExactAuctionMaterialBasket(offers,765,16,16,result) && result.listings.size()==16);
    offers.push_back({17,765,1,1,8});
    assert(!ExactAuctionMaterialBasket(offers,765,17,17,result));
    offers={{1,765,10000,UINT32_MAX,8}};
    assert(ExactAuctionMaterialBasket(offers,765,10000,UINT32_MAX,result) && result.copper==UINT32_MAX);
    offers.push_back({2,765,UINT32_MAX,UINT32_MAX,9});
    assert(ExactAuctionMaterialBasket(offers,765,10000,UINT32_MAX,result) && result.listings.size()==1);
    offers={{1,765,1,UINT32_MAX,8},{2,765,1,UINT32_MAX,9}};
    assert(!ExactAuctionMaterialBasket(offers,765,2,UINT32_MAX,result));
    for(unsigned n=0;n<10;++n) {
        offers={{1,765,1,1,8}};uint32_t entry=765,quantity=1,budget=1;
        switch(n) {
        case 0:entry=0;break;case 1:quantity=0;break;case 2:quantity=10001;break;
        case 3:budget=0;break;case 4:offers[0].id=0;break;case 5:offers[0].entry=1;break;
        case 6:offers[0].quantity=0;break;case 7:offers[0].copper=0;break;
        case 8:offers[0].seller=0;break;case 9:offers.push_back(offers[0]);break;
        }
        assert(!ExactAuctionMaterialBasket(offers,entry,quantity,budget,result));
    }
    // Exhaustive independent subset oracle over small randomized markets.
    // Input order cannot change price, listing identities or tie decisions.
    std::mt19937 random(17092026);
    for(unsigned run=0;run<500;++run) {
        offers.clear();const unsigned count=1+random()%8;
        for(unsigned n=0;n<count;++n)offers.push_back({n+1,765,uint32_t(1+random()%6),uint32_t(1+random()%30),n+20});
        const uint32_t need=1+random()%20,budget=1+random()%100;
        bool found=false;AuctionMaterialBasket expected;
        for(unsigned mask=1;mask<(1u<<count);++mask) {
            uint32_t quantity=0,cost=0;std::vector<uint32_t> ids;
            for(unsigned n=0;n<count;++n)if(mask&(1u<<n)) {
                quantity+=offers[n].quantity;cost+=offers[n].copper;ids.push_back(offers[n].id);
            }
            if(quantity!=need || cost>budget)continue;
            if(!found || cost<expected.copper || (cost==expected.copper &&
                (ids.size()<expected.listings.size() || (ids.size()==expected.listings.size() && ids<expected.listings)))) {
                expected={cost,ids};found=true;
            }
        }
        std::shuffle(offers.begin(),offers.end(),random);
        assert(ExactAuctionMaterialBasket(offers,765,need,budget,result)==found);
        if(found)assert(result.copper==expected.copper && result.listings==expected.listings);
    }
}
