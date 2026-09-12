#include "botpch.h"
#include "LivingProfessionNative.h"
#include "LivingProfessionDemand.h"
#include "LivingProfessionVendor.h"
#include "LivingNativeAuctionPurchase.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingNativeCraftCapture.h"
#include "LivingServiceExecution.h"
#include "LivingNativeBankWithdrawal.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

namespace LivingActivity {
    bool InspectNativeProfessionSnapshot(Player& actor,const Task& task,const ProfessionHistory& history,
        uint64_t nowMs,ProfessionSnapshot& snapshot,std::string& blocker) {
        snapshot={};
        auto reject=[&](const std::string& code){blocker=code;return false;};
        if (!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() ||
            !actor.IsInWorld() || actor.IsBeingTeleported() || actor.GetGUIDLow()!=task.actor)
            return reject("profession_snapshot_native_actor_unavailable");
        const auto permission=actor.GetPlayerbotAI()->ActivityPermissions().Inspect();
        if (!permission || !(ReadNativeContext(actor,permission->current.policyRevision,permission->current.boot)==task.context))
            return reject("profession_snapshot_context_changed");
        if (!task.accepted || task.mode!=Mode::Active || task.root!=task.id || !task.parent.empty() ||
            Terminal(task.phase) || !history.complete || history.task!=task.id || history.revision!=task.revision)
            return reject("profession_snapshot_history_not_current");
        ProfessionJob job;
        if (!ValidateProfessionTask(task,blocker) || !DecodeProfessionJob(task.checkpoint.data,job,blocker)) return false;
        snapshot.task=task.id;snapshot.revision=task.revision;snapshot.context=task.context;
        snapshot.attempts=history.attempts;snapshot.unresolvedOperation=history.unresolvedOperation;
        if (history.unresolvedOperation || sLivingActivityCoordinator.DefersNativeSave(task.actor))
            return reject("profession_snapshot_operation_unresolved");
        NativeProfessionDemand demand;
        if (!InspectNativeProfessionDemand(actor,task,demand)) {
            snapshot.nativeReference=demand.nativeReference;
            if(demand.blocker!="profession_material_has_legacy_commitment")return reject(demand.blocker);
            // Still inspect safety/recipe/history for a metadata-only restart
            // rebind. Never turn protected stock into missing purchasable stock.
            snapshot.readinessBlocker=demand.blocker;
        } else snapshot.stock=std::move(demand.stock);
        snapshot.skill=actor.GetSkillValuePure(job.skill);
        snapshot.safe=!ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
            !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor);
        snapshot.retryReady=nowMs && (!task.retryAtMs || nowMs>=task.retryAtMs);
        const auto native=InspectNativeProfessionRecipe(actor,job);
        snapshot.knownRecipe=native.known && native.recipe==job.recipe;
        // Admission-time identity stays immutable. Later native skill changes
        // affect usefulness, never rewrite the accepted recipe or its history.
        auto current=job;current.initialSkill=native.skillValue;
        std::string nativeBlocker;
        if (job.purpose==ProfessionPurpose::SkillGain && native.skillValue>=job.targetSkill) {
            snapshot.useful=false;
            nativeBlocker="profession_target_met_requires_settlement";
        } else snapshot.useful=MatchNativeProfessionRecipe(current,native,nativeBlocker);
        if (!nativeBlocker.empty()) snapshot.blocker=nativeBlocker;
        ItemGainSpec output;
        EnchantSpec enchant;
        // Recipe/output/tool facts remain true after the accepted target is
        // reached. Usefulness decides whether to cast again, not whether to
        // inspect those facts or acknowledge the previous native result.
        if (snapshot.knownRecipe && native.blocker.empty() && (job.operation==ProfessionOperation::EnchantItem ?
            ReadNativeEnchantSpec(actor,job,enchant,nativeBlocker) : ReadNativeCraftOutput(actor,job,output,nativeBlocker))) {
            snapshot.outputPerAttempt=output.quantity;
            ItemPosCountVec positions;
            snapshot.capacity=job.operation==ProfessionOperation::EnchantItem ||
                actor.CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,output.entry,output.quantity)==EQUIP_ERR_OK;
            const auto* spell=sSpellTemplate.LookupEntry<SpellEntry>(job.recipe);
            snapshot.tools=true;
            for (const auto item : spell->Totem)
                if (item && !actor.HasItemCount(item,1,false)) snapshot.tools=false;
            for (const auto category : spell->TotemCategory)
                if (category && !actor.HasItemTotemCategory(category)) snapshot.tools=false;
            snapshot.atStation=!spell->RequiresSpellFocus;
            if (spell->RequiresSpellFocus) {
                GameObject* focus=nullptr;
                MaNGOS::GameObjectFocusCheck eligible(&actor,spell->RequiresSpellFocus);
                MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> search(focus,eligible);
                Cell::VisitGridObjects(&actor,search,actor.GetMap()->GetVisibilityDistance());
                snapshot.atStation=focus!=nullptr; // Native spawn/distance check, no stored pointer.
            }
        } else if (snapshot.blocker.empty()) snapshot.blocker=nativeBlocker;
        snapshot.bankAccess=NativeNearbyBanker(actor)!=0;
        // Real spawned seller candidates, not generic vendors. Collection of
        // owned/paid stock remains ahead of acquiring anything new.
        for (size_t i=0;i<job.reagents.size() && snapshot.blocker.empty() && snapshot.readinessBlocker.empty();++i) {
            auto& have=snapshot.stock[i];
            if (have.bag>=job.reagents[i].perAttempt) continue;
            if(have.bank || have.delivered) continue;
            const auto* item=sObjectMgr.GetItemPrototype(have.entry);uint32_t quantity=0;
            std::vector<int32_t> sellers;
            have.sourceAvailable=item && RequiredProfessionVendorQuantity(job.reagents[i],have,item->BuyCount,quantity,have.sourceBlocker) &&
                NativeProfessionVendorSources(actor,have.entry,quantity,sellers,have.sourceBlocker);
            if(!have.sourceAvailable && RequiredProfessionVendorQuantity(job.reagents[i],have,1,quantity,have.sourceBlocker))
                have.sourceAvailable=NativeAuctionSourceAvailable(actor,have.entry,quantity,have.sourceBlocker);
        }
        // Completion evidence is evaluated before readiness blockers by the
        // finite policy. A successful receipt must not cause another craft just
        // because its inputs are now gone. Finalize still requires settlement.
        snapshot.complete=true;blocker.clear();return true;
    }
}
