#include "LivingGatherQuote.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    NativeGatherQuote q{45,2770,186,2575,1,75,75,80,100,0,12345},decoded;
    const auto json=EncodeNativeGatherQuote(q);
    assert(DecodeNativeGatherQuote(json,decoded) && EncodeNativeGatherQuote(decoded)==json);
    for(unsigned field=0;field<10;++field) {
        auto bad=q;
        switch(field) {
        case 0:bad.actor=0;break;case 1:bad.entry=0;break;case 2:bad.source=0;break;
        case 3:bad.skill=356;break;case 4:bad.spell=2366;break;case 5:bad.required=81;break;
        case 6:bad.maximum=74;break;case 7:bad.maximum=65536;break;case 8:bad.value=0;break;
        default:bad.effective=65536;break;
        }
        assert(!ValidNativeGatherQuote(bad));
    }
    auto herb=q;herb.skill=182;herb.spell=2366;assert(ValidNativeGatherQuote(herb));
    for(const auto& bad:{"{}", "[]", "null", "{\"actor\":1,\"actor\":2}"})assert(!DecodeNativeGatherQuote(bad,decoded));
    for(const auto& value:{"-1","01","18446744073709551616","null","{}","[]"}) {
        auto bad=json;bad.replace(bad.find("12345"),5,value);assert(!DecodeNativeGatherQuote(bad,decoded));
    }
    auto extra=json;extra.insert(extra.size()-1,",\"quantity\":4");assert(!DecodeNativeGatherQuote(extra,decoded));
    auto duplicate=json;duplicate.insert(duplicate.size()-1,",\"entry\":2770");assert(!DecodeNativeGatherQuote(duplicate,decoded));
    std::string why;
    NativeGatherResult r{q,75,75,100,0,456,true,true,true,true,true,false};
    assert(VerifyNativeGatherResult(r,why)==OperationState::Verified && why=="native_gather_opened_loot_not_collected");
    // Gray/capped nodes are valid material work but never claim skill gains.
    auto skillup=r;skillup.before.maximum=skillup.maximum=150;++skillup.value;
    assert(VerifyNativeGatherResult(skillup,why)==OperationState::Verified);
    for(unsigned field=0;field<12;++field) {
        auto bad=r;
        switch(field) {
        case 0:bad.uncertain=true;break;case 1:bad.started=false;break;case 2:bad.finished=false;break;
        case 3:bad.effect=false;break;case 4:bad.succeeded=false;break;case 5:bad.generation=0;break;
        case 6:bad.owned=false;break;case 7:++bad.money;break;case 8:++bad.bagCount;break;
        case 9:--bad.value;break;case 10:++bad.value;break;default:++bad.maximum;break;
        }
        assert(VerifyNativeGatherResult(bad,why)==OperationState::Reconciling);
    }
    auto rejected=r;rejected.effect=false;rejected.succeeded=false;rejected.generation=0;rejected.owned=false;
    assert(VerifyNativeGatherResult(rejected,why)==OperationState::Rejected);
    rejected.effect=true;assert(VerifyNativeGatherResult(rejected,why)==OperationState::Rejected); // Native resisted lock, no change.
    assert(EncodeNativeGatherResult(r).find("\"bag_count\":0")!=std::string::npos);
}
