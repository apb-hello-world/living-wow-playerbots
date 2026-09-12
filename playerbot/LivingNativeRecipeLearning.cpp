#include "playerbot/playerbot.h"
#include "LivingNativeRecipeLearning.h"
#include "LivingUsefulRecipe.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingActivityGameplay.h"
#include "LivingNativeMailCollection.h"
#include "Mails/Mail.h"
#include "strategy/values/ItemUsageValue.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "Spells/Spell.h"
#include "Spells/Scripts/SpellScript.h"
#include <chrono>
#include <stdexcept>

namespace LivingActivity {
    bool BuildNativeRecipeLearningJob(Player& actor,uint32_t entry,RecipeLearningJob& job,std::string& blocker) {
        job={};auto reject=[&](const char* why){blocker=why;return false;};
        const auto* item=sObjectMgr.GetItemPrototype(entry);
        const auto facts=InspectUsefulRecipe(actor,item);const auto purpose=EvaluateUsefulRecipe(facts);
        if (!purpose.useful) return reject(purpose.reason);
        if (facts.learnedTier) return reject("recipe_tier_advancement_adapter_required");
        uint32_t teacher=0;
        for (const auto& use:item->Spells) {
            if (!use.SpellId || use.SpellTrigger!=ITEM_SPELLTRIGGER_ON_USE) continue;
            if (teacher || use.SpellCharges!=-1) return reject("recipe_single_consumable_teacher_required");
            teacher=use.SpellId;
        }
        const auto* spell=sSpellTemplate.LookupEntry<SpellEntry>(teacher);
        if (!spell || SpellScriptMgr::GetSpellScript(teacher) || SpellScriptMgr::GetSpellScript(facts.recipe) ||
            IsChanneledSpell(spell)) return reject("recipe_scripted_or_channelled_teacher_unsupported");
        unsigned effects=0;
        for (uint8_t i=0;i<MAX_EFFECT_INDEX;++i) if (spell->Effect[i]) {
            if (spell->Effect[i]!=SPELL_EFFECT_LEARN_SPELL ||
                (teacher!=SPELL_ID_GENERIC_LEARN && spell->EffectTriggerSpell[i]!=facts.recipe))
                return reject("recipe_exact_learning_effect_required");
            ++effects;
        }
        for (uint8_t i=0;i<MAX_SPELL_REAGENTS;++i)
            if (spell->Reagent[i]>0 && spell->ReagentCount[i]>0) return reject("recipe_teacher_extra_resources_unsupported");
        if (effects!=1) return reject("recipe_exact_learning_effect_required");
        job={entry,facts.recipe,facts.skill,teacher};
        if (!ValidRecipeLearningJob(job)) return reject("recipe_native_learning_identity_invalid");
        blocker.clear();return true;
    }
    bool ValidateNativeRecipeLearningTask(Player& actor,const Task& task,std::string& blocker) {
        if (!IsRecipeLearningTask(task)) {blocker.clear();return true;}
        RecipeLearningJob saved,current;
        if (task.actor!=actor.GetGUIDLow() || !ValidateRecipeLearningTask(task,blocker) ||
            !DecodeRecipeLearningJob(task.checkpoint.data,saved,blocker) ||
            !BuildNativeRecipeLearningJob(actor,saved.book,current,blocker)) return false;
        if (EncodeRecipeLearningJob(saved)!=EncodeRecipeLearningJob(current)) {
            blocker="recipe_native_learning_identity_changed";return false;
        }
        blocker.clear();return true;
    }
    bool FindNativeRecipeBook(Player& actor,const Task& task,NativeResourceBalance& selected,std::string& blocker) {
        selected={};RecipeLearningJob job;
        if (!sLivingActivityCoordinator.OnWorldThread() || !ValidateNativeRecipeLearningTask(actor,task,blocker) ||
            !DecodeRecipeLearningJob(task.checkpoint.data,job,blocker)) return false;
        bool ownedButUnavailable=false;unsigned inspected=0;
        auto consider=[&](Item* item,const char* location,uint32_t mail) {
            if (!item || item->GetEntry()!=job.book) return;
            ownedButUnavailable=true;
            if (item->GetOwnerGuid()!=actor.GetObjectGuid() || item->GetCount()!=1 || item->IsInTrade() ||
                sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) || sGuildSupplies.ReservedEntry(task.actor,job.book) ||
                ai::ItemUsageValue::IsNeededForQuest(&actor,job.book,true)) return;
            NativeResourceBalance native{task.actor,item->GetGUIDLow(),job.book,1,0,location,mail};uint32_t available=0;
            if (!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,native,available,blocker) || available!=1) return;
            if (!selected.itemGuid || native.itemGuid<selected.itemGuid) selected=native;
        };
        for (unsigned bank=0;bank!=2;++bank) {
            for (auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",bank ?
                IterateItemsMask::ITERATE_ITEMS_IN_BANK:IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
                if (++inspected>256) {blocker="recipe_book_inventory_snapshot_limit";return false;}
                consider(item,bank?"bank":"bags",0);
            }
            if (selected.itemGuid) {blocker.clear();return true;}
        }
        if (actor.GetMailSize()>256) {blocker="recipe_book_mail_snapshot_limit";return false;}
        for (auto it=actor.GetMailBegin();it!=actor.GetMailEnd();++it) {
            const auto* mail=*it;
            if (!mail || mail->receiverGuid!=actor.GetObjectGuid() || mail->state==MAIL_STATE_DELETED ||
                mail->expire_time<=time(nullptr)) continue;
            for (const auto& attachment:mail->items) if (attachment.item_template==job.book) {
                ownedButUnavailable=true;
                if (mail->COD) continue; // Never accept a COD invoice implicitly.
                consider(actor.GetMItem(attachment.item_guid),"mail",mail->messageID);
            }
        }
        if (selected.itemGuid) {blocker.clear();return true;}
        blocker=ownedButUnavailable?"recipe_owned_book_requires_reconciliation":"recipe_book_not_owned";return false;
    }
    namespace {
        std::string FrameJson(const RecipeLearningFrame& f) {
            return "{\"actor\":"+std::to_string(f.actor)+",\"guid\":"+std::to_string(f.guid)+
                ",\"entry\":"+std::to_string(f.entry)+",\"count\":"+std::to_string(f.count)+
                ",\"bag\":"+std::to_string(f.bag)+",\"slot\":"+std::to_string(f.slot)+
                ",\"money\":"+std::to_string(f.money)+",\"skill\":"+std::to_string(f.skill)+
                ",\"maximum\":"+std::to_string(f.maximum)+",\"known\":"+(f.known?"true":"false")+'}';
        }
        bool ReadFrame(Player& actor,const RecipeLearningJob& job,uint32_t guid,RecipeLearningFrame& result) {
            result={};
            if (!guid || !actor.IsInWorld() || actor.IsBeingTeleported()) return false;
            result.actor=actor.GetGUIDLow();result.guid=guid;result.entry=job.book;
            result.money=actor.GetMoney();result.skill=actor.GetSkillValuePure(job.skill);
            result.maximum=actor.GetSkillMaxPure(job.skill);result.known=actor.HasSpell(job.recipe);
            auto* book=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,guid));
            if (!book) return true; // Absence is evidence only AFTER an observed native cast.
            if (book->GetOwnerGuid()!=actor.GetObjectGuid() || book->GetEntry()!=job.book ||
                !Player::IsInventoryPos(book->GetBagSlot(),book->GetSlot())) return false;
            result.count=book->GetCount();result.slot=book->GetSlot();
            result.bag=book->GetContainer()?book->GetContainer()->GetGUIDLow():0;return true;
        }
        bool ReadyBook(Player& actor,const RecipeLearningJob& job,const ClaimConsumption& use,std::string& blocker) {
            auto reject=[&](const char* why){blocker=why;return false;};
            if (!actor.GetPlayerbotAI() || !actor.IsInWorld() || !actor.GetMap() || actor.GetMap()->IsDungeon() ||
                ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || actor.GetTradeData() ||
                !actor.IsStopped()) return reject("recipe_learning_safety_prerequisite");
            RecipeLearningJob current;RecipeLearningFrame frame;
            if (!BuildNativeRecipeLearningJob(actor,job.book,current,blocker)) return false;
            if (EncodeRecipeLearningJob(current)!=EncodeRecipeLearningJob(job) ||
                !ReadFrame(actor,job,use.before.itemGuid,frame) || !frame.count || frame.known ||
                !ValidResourceClaim(use.before) || use.before.actor!=actor.GetGUIDLow() || use.before.itemEntry!=job.book ||
                use.before.location!="bags" || use.before.state!="held" || use.before.quantity!=1 ||
                use.before.copper || use.before.nativeReference || use.used!=1)
                return reject("recipe_learning_exact_carried_book_claim_required");
            const auto protectedItems=sLivingActivityCoordinator.ResourceReservations().Inspect();
            const auto trade=sPlayerbotActionBroker.ReservedItemsView();const auto supply=sGuildSupplies.ReservedItemsView();
            if (!protectedItems || !protectedItems->ready || protectedItems->HasUncertainItem(frame.actor,job.book) ||
                protectedItems->ProtectedItem(frame.guid)!=1 || trade->Item(frame.guid) || supply->Item(frame.guid) ||
                supply->Entry(frame.actor,job.book)) return reject("recipe_learning_other_obligation_protects_book");
            blocker.clear();return true;
        }
        class NativeRecipeLearningCast final : public NativeCraftCast {
        public:
            NativeRecipeLearningCast(Task task,ActionContext action,RecipeLearningJob job,ClaimConsumption use)
                : task(std::move(task)),action(std::move(action)),job(job),use(std::move(use)) {
                std::string blocker;
                if (!ValidateRecipeLearningTask(this->task,blocker) || this->task.phase!=Phase::Executing ||
                    this->task.mode!=Mode::Active || !this->task.accepted || this->use.before.task!=this->task.root ||
                    !Fresh(this->task,this->action,this->task.context) || this->action.revision!=this->task.revision ||
                    EncodeRecipeLearningJob(job)!=this->task.checkpoint.data || (SpellEffectMask(false)&~this->action.permittedEffects))
                    throw std::invalid_argument("recipe_learning_saved_binding_required");
            }
            bool Start(Player& actor,std::string& blocker) override {
                if (result.started || !sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=task.actor ||
                    !Authority(actor) || actor.IsNonMeleeSpellCasted(false,true,true) || !ReadyBook(actor,job,use,blocker) ||
                    !ReadFrame(actor,job,use.before.itemGuid,result.before)) return false;
                auto* book=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,use.before.itemGuid));
                const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(job.teacher);
                if (!book || !info) {blocker="recipe_learning_book_or_teacher_missing";return false;}
                std::unique_ptr<Spell> spell(new Spell(&actor,info,false));
                spell->m_clientCast=true;
                if (job.teacher==SPELL_ID_GENERIC_LEARN) spell->m_currentBasePoints[EFFECT_INDEX_0]=job.recipe;
                spell->SetCastItem(book);book->SetUsedInSpell(true);
                if (!spell->SetLivingCraftCast(shared_from_this())) {blocker="recipe_learning_binding_failed";return false;}
                result.started=true;
                SpellCastTargets targets;targets.setUnitTarget(&actor);
                spell.release()->SpellStart(&targets);blocker.clear();return true;
            }
            bool Ready() const override {return result.finished || result.uncertain;}
            NativeObservation Observe(Player& actor,std::vector<VerifiedItemGain>& gains) const override {
                gains.clear();NativeObservation observation;
                observation.nativeReference="spell:"+std::to_string(job.recipe)+":book:"+std::to_string(use.before.itemGuid);
                observation.afterState="{\"recipe\":"+std::to_string(job.recipe)+",\"skill_id\":"+std::to_string(job.skill)+
                    ",\"teacher\":"+std::to_string(job.teacher)+",\"effect_entered\":"+(result.effect?"true":"false")+
                    ",\"native_finished\":"+(result.finished?"true":"false")+",\"native_succeeded\":"+(result.succeeded?"true":"false")+
                    ",\"before\":"+FrameJson(result.before)+",\"after\":"+FrameJson(result.after)+'}';
                observation.state=VerifyRecipeLearning(job,result,observation.evidence);
                RecipeLearningFrame current;
                if (!ReadFrame(actor,job,use.before.itemGuid,current) || !(current==result.after)) {
                    observation.state=OperationState::Reconciling;observation.evidence="recipe_learning_changed_before_save";
                }
                return observation;
            }
            std::string PersistedProof(Player& actor,const Task& outcome) const override {
                RecipeLearningFrame current;
                if (!result.finished || result.uncertain || !ReadFrame(actor,job,use.before.itemGuid,current) || !(current==result.after))
                    throw std::runtime_error("recipe_learning_native_proof_missing");
                const auto& f=result.after;
                std::string proof="SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
                    " FROM characters WHERE guid="+std::to_string(task.actor)+" AND money="+std::to_string(f.money)+
                    " AND EXISTS (SELECT 1 FROM character_skills WHERE guid="+std::to_string(task.actor)+
                    " AND skill="+std::to_string(job.skill)+" AND value="+std::to_string(f.skill)+" AND max="+std::to_string(f.maximum)+')'+
                    (f.known?" AND EXISTS (":" AND NOT EXISTS (")+"SELECT 1 FROM character_spell WHERE guid="+
                    std::to_string(task.actor)+" AND spell="+std::to_string(job.recipe)+" AND disabled=0)";
                if (f.count) proof+=" AND EXISTS (SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
                    std::to_string(task.actor)+" AND i.guid="+std::to_string(f.guid)+" AND i.owner_guid="+std::to_string(task.actor)+
                    " AND i.itemEntry="+std::to_string(job.book)+" AND i.count="+std::to_string(f.count)+
                    " AND v.bag="+std::to_string(f.bag)+" AND v.slot="+std::to_string(f.slot)+')';
                else proof+=" AND NOT EXISTS (SELECT 1 FROM item_instance WHERE guid="+std::to_string(f.guid)+
                    ") AND NOT EXISTS (SELECT 1 FROM character_inventory WHERE item="+std::to_string(f.guid)+')';
                return proof;
            }
            std::unique_ptr<ExecutionScope> EnterEffect(Spell& spell) override {
                auto* actor=Actor(spell);std::string blocker;RecipeLearningFrame current;
                if (!actor || !result.started || result.effect || result.finished || !Authority(*actor) ||
                    !ReadyBook(*actor,job,use,blocker) || !ReadFrame(*actor,job,use.before.itemGuid,current) || !(current==result.before)) return {};
                result.effect=true;return std::make_unique<ExecutionScope>(task,action);
            }
            void Created(Spell&,uint32_t,uint32_t) noexcept override {result.uncertain=true;}
            void Finished(Spell& spell,bool succeeded) noexcept override {
                try {
                    if (result.finished) return;
                    auto* actor=Actor(spell);
                    if (!actor || !ReadFrame(*actor,job,use.before.itemGuid,result.after)) result.uncertain=true;
                    result.finished=true;result.succeeded=succeeded;
                } catch (...) {result.uncertain=true;}
            }
            void Abandon() noexcept override {if (result.started && !result.finished) result.uncertain=true;}
        private:
            Player* Actor(Spell& spell) const {
                auto* caster=spell.GetTrueCaster();
                return caster && caster->IsPlayer() && caster->GetGUIDLow()==task.actor && spell.m_spellInfo &&
                    spell.m_spellInfo->Id==job.teacher ? static_cast<Player*>(caster):nullptr;
            }
            bool Authority(Player& actor) const {
                auto* ai=actor.GetPlayerbotAI();if (!ai) return false;
                const auto reader=ai->ActivityPermissions();const auto view=reader.Inspect();if (!view) return false;
                const auto current=ReadNativeContext(actor,view->current.policyRevision,view->current.boot);
                const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                return reader.Check({SpellEffectMask(false),Lane::Managed,true},current,now,&task,&action,nullptr,
                    ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)))==AuthorityCode::Allowed;
            }
            const Task task;const ActionContext action;const RecipeLearningJob job;const ClaimConsumption use;
            RecipeLearningResult result;
        };
    }
    uint32_t NativeRecipeLearningOperation::OperationEffects() const {return SpellEffectMask(false);}
    bool NativeRecipeBookReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) {
        RecipeLearningJob job;
        if (!ValidateNativeRecipeLearningTask(actor,request.transition.task,blocker) ||
            !DecodeRecipeLearningJob(request.transition.task.checkpoint.data,job,blocker)) return false;
        if (request.changes.size()!=1) {blocker="recipe_single_book_reservation_required";return false;}
        const auto& change=request.changes.front();const auto& claim=change.after;
        if (change.expectedRevision || claim.revision!=1 || claim.state!="held" ||
            claim.quantity!=1 || claim.copper || claim.task!=request.transition.task.root ||
            claim.actor!=actor.GetGUIDLow() || claim.itemEntry!=job.book || !claim.itemGuid) {
            blocker="recipe_existing_book_required";return false;
        }
        if (claim.location=="mail") {
            NativeResourceBalance balance;
            if (!ReadNativeMailBalance(actor,claim,balance) || actor.GetMail(uint32_t(claim.nativeReference))->COD) {
                blocker="recipe_original_mail_book_required";return false;
            }
        } else {
            const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,claim.itemGuid));
            if (claim.nativeReference || !item || item->GetOwnerGuid()!=actor.GetObjectGuid() ||
                item->GetEntry()!=job.book || item->GetCount()!=1 ||
                !((claim.location=="bags" && Player::IsInventoryPos(item->GetBagSlot(),item->GetSlot())) ||
                  (claim.location=="bank" && Player::IsBankPos(item->GetBagSlot(),item->GetSlot())))) {
                blocker="recipe_original_owned_book_required";return false;
            }
        }
        blocker.clear();return true;
    }
    bool NativeRecipeLearningOperation::ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) {
        RecipeLearningJob job;
        if (request.kind!=OperationKind() || request.effects!=OperationEffects() || request.persistence!=PersistencePolicy() ||
            !request.itemGain.Empty() || !request.itemTransfer.id.empty() || request.consumption.size()!=1 ||
            request.consumption.front().before.task!=request.transition.task.root ||
            !ValidateNativeRecipeLearningTask(actor,request.transition.task,blocker) ||
            !DecodeRecipeLearningJob(request.transition.task.checkpoint.data,job,blocker)) {
            if (blocker.empty()) blocker="recipe_learning_exact_operation_required";return false;
        }
        if (actor.IsNonMeleeSpellCasted(false,true,true)) {blocker="recipe_learning_other_cast_active";return false;}
        return ReadyBook(actor,job,request.consumption.front(),blocker);
    }
    std::shared_ptr<NativeCraftCast> NativeRecipeLearningOperation::ReserveNativeCast(const OperationRequest& request,
        const Task& executing,const ActionContext& action) const {
        RecipeLearningJob job;std::string blocker;
        if (request.consumption.size()!=1 || !DecodeRecipeLearningJob(executing.checkpoint.data,job,blocker))
            throw std::invalid_argument("recipe_learning_exact_operation_required");
        return std::make_shared<NativeRecipeLearningCast>(executing,action,job,request.consumption.front());
    }
}
