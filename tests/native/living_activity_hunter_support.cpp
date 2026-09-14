#include "LivingActivityHunterSupport.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>
using namespace LivingActivity;
struct NativeSpell {
    uint32_t Id=0,SpellFamilyName=0,Effect[3]{},EffectApplyAuraName[3]{},EffectTriggerSpell[3]{},EffectItemType[3]{};
    int32_t EffectMiscValue[3]{},Reagent[8]{};
};
int main(int argc,char**) {
    NativeSpell call;call.Id=883;call.Effect[0]=56;
    NativeSpell revive;revive.Id=982;revive.SpellFamilyName=9;
    revive.Effect[0]=109;revive.Effect[1]=6;revive.EffectApplyAuraName[1]=4;
    for (const auto& spell : {call,revive}) {
        assert(AuditedTbcHunterPetRecovery(spell,3));
        assert(!AuditedTbcHunterPetRecovery(spell,9));
        auto changed=spell;changed.Id=1515;assert(!AuditedTbcHunterPetRecovery(changed,3)); // Tame Beast.
        changed=spell;changed.Effect[0]=28;assert(!AuditedTbcHunterPetRecovery(changed,3)); // Generic summon.
        changed=spell;changed.Effect[2]=24;assert(!AuditedTbcHunterPetRecovery(changed,3));
        changed=spell;changed.EffectMiscValue[0]=416;assert(!AuditedTbcHunterPetRecovery(changed,3));
        changed=spell;changed.EffectTriggerSpell[0]=688;assert(!AuditedTbcHunterPetRecovery(changed,3));
        changed=spell;changed.EffectItemType[1]=6265;assert(!AuditedTbcHunterPetRecovery(changed,3));
        changed=spell;changed.Reagent[7]=6265;assert(!AuditedTbcHunterPetRecovery(changed,3));
        changed=spell;changed.SpellFamilyName=5;assert(!AuditedTbcHunterPetRecovery(changed,3));
    }
    NativeSpell hawk;hawk.Id=13165;hawk.SpellFamilyName=9;
    hawk.Effect[0]=hawk.Effect[1]=6;hawk.EffectApplyAuraName[0]=124;
    hawk.EffectApplyAuraName[1]=42;hawk.EffectTriggerSpell[1]=6150;
    NativeSpell quick;quick.Id=6150;quick.SpellFamilyName=9;quick.Effect[0]=6;quick.EffectApplyAuraName[0]=140;
    for (auto id : {13165u,14318u,14319u,14320u,14321u,14322u,25296u,27044u}) {
        hawk.Id=id;assert(AuditedTbcHunterHawk(hawk,&quick,3));
    }
    assert(!AuditedTbcHunterHawk(hawk,&quick,9));
    assert(!AuditedTbcHunterHawk(hawk,static_cast<NativeSpell*>(nullptr),3));
    for (unsigned variant=0;variant<9;++variant) {
        auto changed=hawk;auto payload=quick;
        switch(variant) {
            case 0:changed.Id=1;break;case 1:changed.EffectTriggerSpell[1]=1;break;
            case 2:payload.Effect[0]=24;break;case 3:payload.EffectApplyAuraName[0]=86;break;
            case 4:payload.EffectTriggerSpell[1]=1;break;case 5:payload.Reagent[0]=6265;break;
            case 6:changed.EffectItemType[0]=6265;break;case 7:payload.EffectMiscValue[0]=1;break;
            default:changed.EffectApplyAuraName[2]=4;break;
        }
        assert(!AuditedTbcHunterHawk(changed,&payload,3));
    }
    if(argc>1) {
        // The Python harness streams actual private TBC DBC records, not
        // translated spell descriptions or reconstructed source patches.
        std::map<uint32_t,NativeSpell> records;
        for(NativeSpell row;std::cin>>row.Id>>row.SpellFamilyName;) {
            for(auto& value:row.Effect)std::cin>>value;
            for(auto& value:row.EffectApplyAuraName)std::cin>>value;
            for(auto& value:row.EffectTriggerSpell)std::cin>>value;
            for(auto& value:row.EffectItemType)std::cin>>value;
            for(auto& value:row.EffectMiscValue)std::cin>>value;
            for(auto& value:row.Reagent)std::cin>>value;
            assert(std::cin);records.emplace(row.Id,row);
        }
        assert(records.size()==11);
        assert(AuditedTbcHunterPetRecovery(records.at(883),3));
        assert(AuditedTbcHunterPetRecovery(records.at(982),3));
        for(auto id:{13165u,14318u,14319u,14320u,14321u,14322u,25296u,27044u})
            assert(AuditedTbcHunterHawk(records.at(id),&records.at(6150),3));
    }
}
