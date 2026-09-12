#pragma once
#include "LivingCraftCapture.h"
#include <algorithm>
#include <tuple>

namespace LivingActivity {
    struct EnchantmentValue {
        uint32_t id=0, duration=0, charges=0;
        bool operator==(const EnchantmentValue& other) const {
            return std::tie(id,duration,charges)==std::tie(other.id,other.duration,other.charges);
        }
    };
    struct EnchantSubject {
        NativeItemStack item;
        std::vector<EnchantmentValue> enchantments;
    };
    struct EnchantSpec { uint32_t id=0; uint8_t slot=0; };
    inline bool SameEnchantSubject(const EnchantSubject& a,const EnchantSubject& b,bool values=true) {
        const auto& x=a.item;const auto& y=b.item;
        return std::tie(x.actor,x.guid,x.entry,x.count,x.bagGuid,x.slot)==
            std::tie(y.actor,y.guid,y.entry,y.count,y.bagGuid,y.slot) &&
            (!values || a.enchantments==b.enchantments);
    }
    inline bool ValidEnchantSubject(const EnchantSubject& subject,uint32_t actor,uint32_t guid,const EnchantSpec& spec) {
        const auto& item=subject.item;
        // This adapter is permanent enchanting only; the native producer must
        // independently validate PERM_ENCHANTMENT_SLOT and the exact spell effect.
        return actor && guid && item.actor==actor && item.guid==guid && item.entry && item.count==1 &&
            spec.id && spec.slot==0 && !subject.enchantments.empty() && subject.enchantments.size()<=16;
    }
    inline CraftVerification VerifyEnchantResources(uint32_t actor,const ProfessionJob& job,const EnchantSpec& spec,
        const CraftFrame& before,const CraftFrame& after,const EnchantSubject& subjectBefore,const EnchantSubject& subjectAfter) {
        CraftVerification result;std::string blocker;
        auto reject=[&](const std::string& code){result.blocker=code;return result;};
        if (!ValidateProfessionJob(job,blocker) || job.operation!=ProfessionOperation::EnchantItem ||
            !ValidEnchantSubject(subjectBefore,actor,job.subjectItem,spec) ||
            !ValidEnchantSubject(subjectAfter,actor,job.subjectItem,spec) ||
            !SameEnchantSubject(subjectBefore,subjectAfter,false) ||
            subjectBefore.enchantments.size()!=subjectAfter.enchantments.size())
            return reject("native_enchant_subject_identity_changed");
        for (const auto& need:job.reagents)
            if (need.entry==subjectBefore.item.entry) return reject("native_enchant_subject_is_reagent");
        for (const auto* frame:{&before,&after})
            for (const auto& stack:frame->stacks)
                if (stack.guid==job.subjectItem || (stack.bagGuid==subjectBefore.item.bagGuid && stack.slot==subjectBefore.item.slot))
                    return reject("native_enchant_subject_input_collision");
        for (size_t i=0;i<subjectBefore.enchantments.size();++i) {
            const auto& changed=subjectAfter.enchantments[i];
            if (i==spec.slot) {
                if (changed.id!=spec.id || changed.duration || changed.charges)
                    return reject("native_enchant_expected_effect_missing");
            } else if (!(changed==subjectBefore.enchantments[i])) return reject("native_enchant_unrelated_effect_changed");
        }
        if (job.purpose==ProfessionPurpose::Equipment && SameEnchantSubject(subjectBefore,subjectAfter))
            return reject("native_enchant_equipment_unchanged");
        if (!VerifyCraftReagents(actor,job.reagents,before,after,blocker)) return reject(blocker);
        result.result=CraftEvidence::Verified;result.blocker.clear();
        result.attempt.recipe=job.recipe;result.attempt.subjectItem=job.subjectItem;result.attempt.consumed=job.reagents;
        result.attempt.skillBefore=before.skill;result.attempt.skillAfter=after.skill;
        return result; // Physical conservation alone is neither callback nor commit proof.
    }
    inline CraftVerification VerifyEnchantCapture(const CraftIdentity& expected,const ProfessionJob& job,const EnchantSpec& spec,
        const CraftCaptureResult& observed,const EnchantSubject& before,const EnchantSubject& after) {
        CraftVerification result;std::string blocker;
        auto reject=[&](const std::string& code){result.blocker=code;return result;};
        if (!ValidCraftIdentity(expected) || !SameCraftIdentity(expected,observed.identity) ||
            !ValidateProfessionJob(job,blocker) || job.recipe!=expected.recipe || job.skill!=expected.skill ||
            job.operation!=ProfessionOperation::EnchantItem)
            return reject("native_enchant_identity_mismatch");
        if (!observed.blocker.empty()) return reject(observed.blocker);
        if (observed.phase!=CraftCapturePhase::Finished || !observed.nativeFinished ||
            !ValidCraftFrame(observed.before) || !ValidCraftFrame(observed.after) ||
            observed.before.actor!=expected.world.actor || observed.after.actor!=expected.world.actor ||
            !ValidEnchantSubject(before,expected.world.actor,job.subjectItem,spec) ||
            !ValidEnchantSubject(after,expected.world.actor,job.subjectItem,spec))
            return reject("native_enchant_completion_proof_missing");
        if (observed.createdCalls || observed.createdEntry || observed.createdQuantity)
            return reject("native_enchant_unexpected_item_creation");
        if (!observed.nativeSucceeded) {
            if (!observed.effectEntered && SameCraftFrame(observed.before,observed.after) && SameEnchantSubject(before,after)) {
                result.result=CraftEvidence::RejectedWithoutEffect;result.blocker="native_cast_cancelled_without_effect";return result;
            }
            return reject("native_enchant_cancelled_after_possible_effect");
        }
        if (!observed.effectEntered) return reject("native_enchant_effect_callback_missing");
        result=VerifyEnchantResources(expected.world.actor,job,spec,observed.before,observed.after,before,after);
        if (result.result==CraftEvidence::Verified) result.attempt.nativeEffectVerified=true;
        return result;
    }
}
