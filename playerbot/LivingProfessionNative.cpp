#include "playerbot/playerbot.h"
#include "LivingProfessionNative.h"
#include "LivingCommissionJob.h"
#include "LivingProfessionPlan.h"
#include "LivingNativeCraftCapture.h"
#include "LivingActivityCoordinator.h"
#include "LivingProfessionTools.h"
#include "LivingTaskItemRequirements.h"
#include "LivingProfessionVendor.h"
#include "LivingNativeAuctionPurchase.h"
#include "ServerFacade.h"
#include "Spells/SpellMgr.h"
#include <map>
#include <set>

namespace LivingActivity {
    namespace {
        std::vector<std::pair<uint32_t,uint32_t>> toolCatalog;
        std::map<uint32_t,std::vector<uint32_t>> toolRecipes;
        bool toolCatalogReady=false;
    }
    void BuildNativeProfessionToolCatalog() {
        toolCatalog.clear();toolRecipes.clear();toolCatalogReady=false;
        for (uint32_t id=1;id<sItemStorage.GetMaxEntry();++id) {
            const auto* item=sObjectMgr.GetItemPrototype(id);
            if (item && item->TotemCategory) toolCatalog.emplace_back(id,item->TotemCategory);
        }
        for(uint32_t id=1;id<sSpellTemplate.GetMaxEntry();++id) {
            const auto* spell=sServerFacade.LookupSpellInfo(id);if(!spell)continue;
            for(uint8_t i=0;i<MAX_EFFECT_INDEX;++i)
                if(spell->Effect[i]==SPELL_EFFECT_CREATE_ITEM && spell->EffectItemType[i]) {
                    auto& recipes=toolRecipes[spell->EffectItemType[i]];
                    if(recipes.empty() || recipes.back()!=id)recipes.push_back(id);
                }
        }
        toolCatalogReady=true;
    }
    namespace {
        bool IsNativeRequiredTool(const ProfessionJob& parent,uint32_t entry) {
            const auto* spell=sServerFacade.LookupSpellInfo(parent.recipe);
            const auto* item=sObjectMgr.GetItemPrototype(entry);if(!spell || !item)return false;
            for(const auto exact:spell->Totem)if(exact && exact==entry)return true;
            for(const auto category:spell->TotemCategory)
                if(category && item->TotemCategory && IsTotemCategoryCompatiableWith(item->TotemCategory,category))return true;
            return false;
        }
    }
    bool BuildNativeProfessionToolPreparation(Player& actor,const Task& task,uint32_t entry,
        ProfessionWorkflow& result,std::string& blocker) {
        result={};auto reject=[&](const char* why){blocker=why;return false;};
        ProfessionWorkflow flow;
        if(!sLivingActivityCoordinator.OnWorldThread() || !toolCatalogReady || actor.GetGUIDLow()!=task.actor ||
            !actor.IsInWorld() || actor.IsBeingTeleported() || task.root!=task.id || !task.accepted ||
            !DecodeProfessionWorkflow(task.checkpoint.data,flow,blocker))return reject("profession_tool_preparation_context_invalid");
        if(!flow.tools.empty() && !flow.tools.back().finishedRevision)return reject("profession_tool_nested_preparation_required");
        if(flow.tools.size()>=4)return reject("profession_tool_preparation_limit");
        if(!IsNativeRequiredTool(flow.intent,entry))return reject("profession_tool_not_required_by_parent");
        if(actor.HasItemCount(entry,1,true))return reject("profession_tool_already_owned_requires_collection");
        for(const auto& tool:flow.tools)if(tool.job.outputEntry==entry)return reject("profession_prepared_tool_requires_reconciliation");
        const auto found=toolRecipes.find(entry);
        if(found==toolRecipes.end())return reject("profession_tool_crafting_recipe_unavailable");
        for(const auto id:found->second) {
            ProfessionJob job;job.recipe=id;job.skill=flow.intent.skill;job.purpose=ProfessionPurpose::Intermediate;
            const auto native=InspectNativeProfessionRecipe(actor,job);
            if(!native.blocker.empty() || native.operation!=ProfessionOperation::CreateItem || native.outputEntry!=entry)continue;
            job.reagents=native.reagents;job.outputEntry=entry;job.outputQuantity=1;job.initialSkill=native.skillValue;
            job.targetSkill=0;job.attemptLimit=3;
            ItemGainSpec output;std::string why;
            if(!MatchNativeProfessionRecipe(job,native,why) || !ReadNativeCraftOutput(actor,job,output,why) || output.quantity!=1)continue;
            // This finite adapter creates a tool, not a recursive crafting DSL.
            // A missing prerequisite of that tool is explicit until supported.
            const auto* spell=sServerFacade.LookupSpellInfo(id);bool ready=true;
            for(const auto tool:spell->Totem)if(tool && !actor.HasItemCount(tool,1,false))ready=false;
            for(const auto category:spell->TotemCategory)if(category && !actor.HasItemTotemCategory(category))ready=false;
            if(!ready)continue;
            flow.tools.push_back({job,task.revision+1,0});result=std::move(flow);blocker.clear();return true;
        }
        return reject("profession_tool_known_executable_recipe_unavailable");
    }
    bool ReadNativeProfessionTools(Player& actor,const Task& task,
        std::vector<ProfessionReagent>& tools,std::string& unavailable,std::string& blocker) {
        tools.clear();unavailable.clear();
        auto reject=[&](const char* why){blocker=why;return false;};
        if (!sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=task.actor || !actor.IsInWorld() ||
            actor.IsBeingTeleported() || !toolCatalogReady) return reject("profession_tool_catalog_or_context_unavailable");
        ProfessionJob job;
        if (!DecodeProfessionJob(task.checkpoint.data,job,blocker)) return false;
        const auto* spell=sServerFacade.LookupSpellInfo(job.recipe);
        if (!spell) return reject("profession_tool_recipe_unavailable");
        // A tentative transition may be one revision ahead; claims always come
        // from the acknowledged root with the identical immutable checkpoint.
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(task.id);
        UnsettledClaimBatch claims;
        if (!saved || saved->actor!=task.actor || saved->checkpoint.data!=task.checkpoint.data ||
            !sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,claims,blocker))
            return reject("profession_tool_claim_snapshot_required");
        std::set<uint32_t> selected;
        for (const auto entry:spell->Totem) if (entry) {
            if (!sObjectMgr.GetItemPrototype(entry)) return reject("profession_tool_native_item_unavailable");
            selected.insert(entry);
        }
        for (const auto category:spell->TotemCategory) if (category) {
            std::vector<ProfessionToolCandidate> candidates;
            for (const auto& item:toolCatalog) {
                if (!IsTotemCategoryCompatiableWith(item.second,category)) continue;
                if (candidates.size()>=64) return reject("profession_tool_category_snapshot_bound");
                const auto* proto=sObjectMgr.GetItemPrototype(item.first);
                if (!proto) continue;
                ProfessionToolCandidate candidate;candidate.entry=item.first;
                candidate.carried=actor.GetItemCount(item.first,false)>0;
                candidate.banked=actor.GetItemCount(item.first,true)>actor.GetItemCount(item.first,false);
                for (const auto& claim:claims.claims)
                    if (claim.itemEntry==item.first && claim.quantity &&
                        (claim.location=="bags" || claim.location=="bank" || claim.location=="mail")) candidate.committed=true;
                if (!(candidate.committed || candidate.carried || candidate.banked) && actor.CanUseItem(proto)!=EQUIP_ERR_OK) continue;
                candidates.push_back(candidate);
            }
            // Do not query the market when an owned/committed compatible tool
            // already fulfils this category. Preserve paid incoming identity.
            auto entry=ChooseProfessionTool(candidates);
            if (!entry) {
                for (auto& candidate:candidates) {
                    const auto* proto=sObjectMgr.GetItemPrototype(candidate.entry);
                    std::vector<int32_t> vendors;std::string why;
                    candidate.vendor=proto && proto->BuyCount==1 &&
                        NativeProfessionVendorSources(actor,candidate.entry,1,vendors,why);
                }
                entry=ChooseProfessionTool(candidates);
                if (!entry) {
                    for (auto& candidate:candidates) {
                        std::string why;candidate.auction=NativeAuctionSourceAvailable(actor,candidate.entry,1,why);
                        if (why=="profession_purchase_market_snapshot_busy") {blocker=why;return false;}
                    }
                    entry=ChooseProfessionTool(candidates);
                }
            }
            // A real compatible catalog item with no currently obtainable
            // source remains a truthful shortage, never permission to spawn it.
            if (!entry && !candidates.empty()) entry=candidates.front().entry;
            if (entry) selected.insert(entry);
            else unavailable="profession_tool_category_has_no_usable_item";
        }
        for (const auto entry:selected) tools.push_back({entry,1});
        if (!ValidProfessionTools(job,tools)) return reject("profession_tool_consumable_overlap_or_invalid");
        blocker.clear();return true;
    }
    bool ReadNativeTaskItemRequirements(Player& actor,const Task& task,
        std::vector<ProfessionReagent>& items,std::string& blocker,std::string* unavailable) {
        if (unavailable) unavailable->clear();
        if (!ReadTaskItemRequirements(task,items,blocker)) return false;
        if (!IsProfessionJob(task)) return true;
        std::vector<ProfessionReagent> tools;std::string missing;
        if (!ReadNativeProfessionTools(actor,task,tools,missing,blocker)) return false;
        if (unavailable) *unavailable=missing;
        items.insert(items.end(),tools.begin(),tools.end());return true;
    }
    bool BuildNativeSkillGainJob(Player& actor,uint32_t recipe,ProfessionJob& job,std::string& blocker) {
        job={};job.recipe=recipe;
        auto reject=[&](const char* why){blocker=why;return false;};
        const auto bounds=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(recipe);
        for (auto it=bounds.first;it!=bounds.second;++it) {
            const auto* line=it->second;
            if (!line || !actor.GetSkillValuePure(line->skillId) ||
                (!LivingProfessions::Primary(line->skillId) && line->skillId!=SKILL_COOKING && line->skillId!=SKILL_FIRST_AID)) continue;
            if (job.skill && job.skill!=line->skillId) return reject("profession_skill_identity_ambiguous");
            job.skill=line->skillId;
        }
        const auto native=InspectNativeProfessionRecipe(actor,job);
        if (!native.blocker.empty()) {blocker=native.blocker;return false;}
        if (native.operation!=ProfessionOperation::CreateItem && native.operation!=ProfessionOperation::EnchantItem)
            return reject("profession_subject_executor_required");
        job.initialSkill=native.skillValue;job.targetSkill=native.skillValue+1;
        job.operation=native.operation;job.purpose=ProfessionPurpose::SkillGain;
        job.outputEntry=native.outputEntry;job.outputQuantity=native.operation==ProfessionOperation::CreateItem?1:0;job.reagents=native.reagents;
        if (job.operation==ProfessionOperation::EnchantItem) {
            if (!actor.GetPlayerbotAI()) return reject("profession_native_actor_unavailable");
            // Prefer an owned equipped piece. Never replace a different existing
            // permanent enchant merely to practice a low-level skill recipe.
            for (const auto location:{IterateItemsMask::ITERATE_ITEMS_IN_EQUIP,IterateItemsMask::ITERATE_ITEMS_IN_BAGS}) {
                uint32_t selected=0;unsigned count=0;
                for (auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",location)) {
                    if (++count>256) return reject("native_enchant_subject_snapshot_bound");
                    if (!item) continue;
                    job.subjectItem=item->GetGUIDLow();EnchantSpec spec;std::string why;
                    if (!ReadNativeEnchantSpec(actor,job,spec,why) ||
                        (item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) && item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)!=spec.id)) continue;
                    if (!selected || job.subjectItem<selected) selected=job.subjectItem;
                }
                job.subjectItem=selected;
                if (selected) break;
            }
            if (!job.subjectItem) return reject("native_enchant_safe_owned_subject_unavailable");
        }
        return MatchNativeProfessionRecipe(job,InspectNativeProfessionRecipe(actor,job),blocker);
    }
    bool NativeRequestedItemRecipes(uint32_t entry,std::vector<uint32_t>& recipes,std::string& blocker) {
        recipes.clear();
        if(!sLivingActivityCoordinator.OnWorldThread() || !toolCatalogReady) {blocker="guild_craft_catalog_not_ready";return false;}
        const auto found=toolRecipes.find(entry);
        if(found==toolRecipes.end()){blocker="guild_craft_no_item_recipe";return false;}
        if(found->second.size()>64){blocker="guild_craft_recipe_snapshot_bound";return false;}
        recipes=found->second;blocker.clear();return true;
    }
    bool BuildNativeRequestedItemJob(Player& actor,uint32_t recipe,uint32_t entry,uint32_t maximum,
        ProfessionJob& job,std::string& blocker) {
        job={};job.recipe=recipe;job.purpose=ProfessionPurpose::RequestedItem;
        auto reject=[&](const char* why){blocker=why;return false;};
        if(!sLivingActivityCoordinator.OnWorldThread() || !maximum)return reject("guild_craft_candidate_context_invalid");
        const auto bounds=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(recipe);
        for(auto it=bounds.first;it!=bounds.second;++it) {
            const auto* line=it->second;
            if(!line || !actor.GetSkillValuePure(line->skillId) ||
                (!LivingProfessions::Primary(line->skillId) && line->skillId!=SKILL_COOKING && line->skillId!=SKILL_FIRST_AID))continue;
            if(job.skill && job.skill!=line->skillId)return reject("profession_skill_identity_ambiguous");
            job.skill=line->skillId;
        }
        const auto native=InspectNativeProfessionRecipe(actor,job);
        if(!native.blocker.empty()){blocker=native.blocker;return false;}
        if(native.operation!=ProfessionOperation::CreateItem || native.outputEntry!=entry || native.reagents.empty())
            return reject("guild_craft_requested_output_unsupported");
        job.initialSkill=native.skillValue;job.outputEntry=entry;job.outputQuantity=1;job.reagents=native.reagents;
        job.targetSkill=0;job.attemptLimit=3;
        ItemGainSpec output;
        if(!MatchNativeProfessionRecipe(job,native,blocker) || !ReadNativeCraftOutput(actor,job,output,blocker))return false;
        // One native batch per accepted request. Native surplus remains owned;
        // it is not silently added to demand, destroyed, or donated twice.
        job.outputQuantity=std::min(maximum,output.quantity);
        return ValidateProfessionJob(job,blocker);
    }
    NativeProfessionRecipe InspectNativeProfessionRecipe(Player& actor, const ProfessionJob& job) {
        NativeProfessionRecipe result;
        auto reject = [&](const char* blocker) { result.blocker = blocker; return result; };
        if (!actor.IsInWorld() || actor.IsBeingTeleported()) return reject("profession_native_actor_unavailable");
        const auto known = actor.GetSpellMap().find(job.recipe);
        const auto* spell = sServerFacade.LookupSpellInfo(job.recipe);
        if (!spell || known == actor.GetSpellMap().end() || known->second.state == PLAYERSPELL_REMOVED ||
            known->second.disabled || IsPassiveSpell(spell)) return reject("profession_recipe_not_known");
        result.recipe = spell->Id; result.known = true;
        // Primary careers and existing secondary professions remain distinct.
        if (!LivingProfessions::Primary(job.skill) && job.skill != SKILL_COOKING &&
            job.skill != SKILL_FIRST_AID && job.skill != SKILL_FISHING)
            return reject("profession_native_skill_unsupported");
        const auto bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spell->Id);
        for (auto it = bounds.first; it != bounds.second; ++it) {
            const auto* line = it->second;
            if (!line || line->skillId != job.skill) continue;
            result.skill = line->skillId;
            result.greyAt = std::max(result.greyAt, uint32(line->max_value));
        }
        if (!result.skill) return reject("profession_recipe_skill_mismatch");
        result.skillValue = actor.GetSkillValuePure(result.skill);
        result.skillMaximum = actor.GetSkillMaxPure(result.skill);
        if (!result.skillValue || !result.skillMaximum) return reject("profession_native_skill_not_learned");
        bool creates = false, enchants = false, disenchants = false;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i) {
            if (!spell->Effect[i]) continue;
            // Unknown scripted/transformation effects need a focused executor,
            // not a guess based on the recipe name or a matching reagent.
            if (spell->EffectTriggerSpell[i]) return reject("profession_native_trigger_unsupported");
            switch (spell->Effect[i]) {
                case SPELL_EFFECT_CREATE_ITEM:
                    if (!spell->EffectItemType[i] || !sObjectMgr.GetItemPrototype(spell->EffectItemType[i]))
                        return reject("profession_native_output_unavailable");
                    if (result.outputEntry && result.outputEntry != spell->EffectItemType[i])
                        return reject("profession_multiple_native_outputs_unsupported");
                    creates = true; result.outputEntry = spell->EffectItemType[i]; break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                    enchants = true; break;
                case SPELL_EFFECT_DISENCHANT:
                    disenchants = true; break;
                default: return reject("profession_native_effect_unsupported");
            }
        }
        if (unsigned(creates) + unsigned(enchants) + unsigned(disenchants) != 1)
            return reject("profession_native_operation_unsupported");
        result.operation = creates ? ProfessionOperation::CreateItem :
            enchants ? ProfessionOperation::EnchantItem : ProfessionOperation::DisenchantItem;
        std::map<uint32_t, uint64_t> ingredients;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i) {
            if (spell->Reagent[i] <= 0) continue;
            if (!spell->ReagentCount[i] || !sObjectMgr.GetItemPrototype(spell->Reagent[i]))
                return reject("profession_native_reagent_unavailable");
            ingredients[uint32_t(spell->Reagent[i])] += spell->ReagentCount[i];
        }
        for (const auto& ingredient : ingredients) {
            if (ingredient.second > 10000) return reject("profession_native_reagent_quantity_unsupported");
            result.reagents.push_back({ingredient.first, uint32_t(ingredient.second)});
        }
        if (job.subjectItem) {
            const auto* subject = actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM, job.subjectItem));
            result.subjectOwned = subject && subject->GetOwnerGuid() == actor.GetObjectGuid();
        }
        result.blocker.clear(); return result;
    }
    bool ValidateNativeProfessionTask(Player& actor, const Task& task, std::string& blocker) {
        if (!IsProfessionJob(task)) { blocker.clear(); return true; }
        if (actor.GetGUIDLow() != task.actor) { blocker = "profession_native_actor_mismatch"; return false; }
        ProfessionJob job;
        if (!ValidateProfessionTask(task, blocker) || !DecodeProfessionJob(task.checkpoint.data, job, blocker)) return false;
        if(IsCommissionJob(task)) {
            CommissionJob commission;
            if(!DecodeCommissionJob(task.checkpoint.data,commission,blocker))return false;
            if(commission.craftFinishedRevision){blocker="commission_craft_already_verified";return false;}
        }
        if(IsGuildCraftTask(task)) {
            GuildProcurementJob guild;
            if(!DecodeGuildProcurementJob(task.checkpoint.data,guild,blocker))return false;
            if(guild.craftFinishedRevision){blocker="guild_craft_already_verified";return false;}
            if(!sLivingActivityCoordinator.ValidateGuildProcurementDemand(task,blocker))return false;
        }
        ProfessionWorkflow flow;if(!DecodeProfessionWorkflow(task.checkpoint.data,flow,blocker))return false;
        for(const auto& tool:flow.tools)
            if(!IsNativeRequiredTool(flow.intent,tool.job.outputEntry)) {blocker="profession_tool_not_required_by_parent";return false;}
        return MatchNativeProfessionRecipe(job, InspectNativeProfessionRecipe(actor, job), blocker);
    }
}
