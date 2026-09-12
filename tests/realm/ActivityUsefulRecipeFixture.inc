// Native use of one ALREADY OWNED book in the copied realm. No item, money,
// spell or skill grant, teleport, purchase, task creation or fake completion.
#include "playerbot/LivingUsefulRecipe.h"
#include "playerbot/strategy/actions/UseItemAction.h"
namespace {
    template<class FixtureState>
    void RunIsolatedUsefulRecipeFixture(FixtureState& state) {
        if (state.fixtureFinished || NowMs()<state.fixtureNext) return;
        state.fixtureNext=NowMs()+500;
        auto& fixture=state.recipeLearningFixture;
        auto emit=[&](bool passed,const std::string& failure,Player* actor=nullptr) {
            boost::property_tree::ptree report,before,after;
            report.put("suite","activity-useful-recipe-v1");report.put("boot",state.boot);
            report.put("test_only",true);report.put("outcome",passed?"passed":"failed");
            report.put("failure",failure);report.put("blocker",fixture.blocker);
            report.put("actor",fixture.actor);report.put("item",fixture.item);report.put("recipe",fixture.recipe);
            report.put("skill",fixture.skill);report.put("skill_before",fixture.skillBefore);
            report.put("money_before",fixture.moneyBefore);report.put("count_before",fixture.countBefore);
            report.put("item_grants",0);report.put("money_grants",0);report.put("spell_grants",0);
            report.put("skill_grants",0);report.put("fixture_relocations",0);report.put("new_tasks",0);
            report.put("known_before",false);
            report.put("managed_learning_receipt",false);report.put("paid_workflow_proof",false);
            report.put("learning_action_admitted",fixture.started);
            report.put("general_execution_enabled",state.enforceEffects.load(std::memory_order_acquire));
            for (const auto& item:fixture.beforeStacks) {
                boost::property_tree::ptree row;row.put("guid",item.first);row.put("quantity",item.second);
                before.push_back({"",row});
            }
            if (actor) {
                report.put("known_after",actor->HasSpell(fixture.recipe));
                report.put("money_after",actor->GetMoney());report.put("skill_after",actor->GetSkillValuePure(fixture.skill));
                report.put("count_after",actor->GetItemCount(fixture.item,false));
                for (auto* item:actor->GetPlayerbotAI()->InventoryParseItems(std::to_string(fixture.item),IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
                    boost::property_tree::ptree row;row.put("guid",item->GetGUIDLow());row.put("quantity",item->GetCount());
                    after.push_back({"",row});
                }
            }
            report.add_child("before_stacks",before);report.add_child("after_stacks",after);
            report.add_child("checks",fixture.checks);
            std::ofstream output("/isolated/evidence/activity-useful-recipe-"+state.boot+".json");
            output<<Json(report);output.close();state.fixtureFinished=true;
            if (!output) sLog.outError("Living useful recipe evidence write failed");
            sLog.outString("Living isolated fixture: %s",Json(report).c_str());
            // Keep this copied actor's competing AI held until evidence export
            // and restart. Native spells/session processing continue normally.
        };
        auto require=[&](bool ok,const char* name) {
            boost::property_tree::ptree row;row.put("case",name);row.put("passed",ok);fixture.checks.push_back({"",row});
            if (!ok) throw std::runtime_error(name);
        };
        try {
            if (!fixture.actor) {
                auto id=[](const char* key) {
                    const char* value=std::getenv(key);const std::string input=value?value:"";
                    if (input.empty() || input.size()>9 || input.find_first_not_of("0123456789")!=std::string::npos)
                        throw std::runtime_error("explicit_native_recipe_fixture_ids_required");
                    return uint32_t(std::stoul(input));
                };
                fixture.actor=id("LIVING_WOW_NATIVE_FIXTURE_ACTOR");fixture.item=id("LIVING_WOW_NATIVE_FIXTURE_ITEM");
                require(fixture.actor && fixture.item,"explicit_copied_actor_and_book");
                // Hold before task loading so ordinary AI cannot consume the
                // very book being inspected while the fixture waits for I/O.
                state.gameplayFixtureActor.store(fixture.actor,std::memory_order_release);
            }
            if (sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
                !state.pending.empty() || !state.incoming.empty() || !state.resources.Protection().ready) return;
            if (!fixture.deadline) fixture.deadline=NowMs()+120000;
            if (NowMs()>fixture.deadline) {emit(false,"native_learning_deadline");return;}
            bool committed=false;
            for (const auto& row:state.cache) if (row.second.actor==fixture.actor &&
                row.second.mode==Mode::Active && !Terminal(row.second.phase)) committed=true;
            if (committed) {emit(false,"accepted_task_would_be_displaced");return;}
            auto* actor=sRandomPlayerbotMgr.GetPlayerBot(fixture.actor);
            if (!actor) {
                if (!fixture.requestedLogin && sRandomPlayerbotMgr.GetAllBots().size()<49) {
                    fixture.requestedLogin=true;sRandomPlayerbotMgr.AddRandomBot(fixture.actor);
                }
                fixture.blocker="native_recipe_actor_login_pending";return;
            }
            require(!actor->isRealPlayer() && actor->GetPlayerbotAI() && actor->GetSession() &&
                sPlayerbotAIConfig.IsInRandomAccountList(actor->GetSession()->GetAccountId()),"native_bot_account_only");
            if (!actor->IsInWorld() || !actor->GetMap() || NativeSafety(actor) || actor->GetGroup() ||
                actor->GetTradeData() || actor->GetMap()->IsDungeon()) {
                fixture.blocker="native_recipe_safety_wait";return;
            }
            auto* ai=actor->GetPlayerbotAI();
            if (!fixture.started) {
                require(!committed,"no_accepted_task_displaced");
                const auto facts=InspectUsefulRecipe(*actor,sObjectMgr.GetItemPrototype(fixture.item));
                const auto decision=EvaluateUsefulRecipe(facts);
                fixture.blocker=decision.reason;
                require(decision.useful && !facts.known,"native_unknown_usable_recipe_has_real_purpose");
                fixture.recipe=facts.recipe;fixture.skill=facts.skill;fixture.skillBefore=facts.skillValue;
                fixture.moneyBefore=actor->GetMoney();fixture.countBefore=actor->GetItemCount(fixture.item,false);
                require(fixture.countBefore>0,"existing_owned_book_not_fixture_grant");
                for (auto* item:ai->InventoryParseItems(std::to_string(fixture.item),IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
                    fixture.beforeStacks.emplace(item->GetGUIDLow(),item->GetCount());
                require(!fixture.beforeStacks.empty(),"exact_original_book_stacks_recorded");
                const bool admitted=ai->DoSpecificAction("use random recipe",Event("isolated_recipe_learning","",actor),true);
                fixture.started=admitted;require(admitted,"ordinary_recipe_action_admitted");
                fixture.blocker="native_learning_cast_pending";return;
            }
            if (actor->IsNonMeleeSpellCasted(false)) return;
            require(actor->HasSpell(fixture.recipe),"native_spellbook_contains_learned_recipe");
            require(actor->GetItemCount(fixture.item,false)+1==fixture.countBefore,"one_owned_recipe_book_consumed");
            bool conserved=true;
            for (auto* item:ai->InventoryParseItems(std::to_string(fixture.item),IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
                const auto original=fixture.beforeStacks.find(item->GetGUIDLow());
                if (original==fixture.beforeStacks.end() || item->GetCount()>original->second) conserved=false;
            }
            require(conserved,"surviving_recipe_books_keep_original_identity");
            require(actor->GetMoney()==fixture.moneyBefore,"learning_did_not_generate_or_spend_money");
            require(actor->GetSkillValuePure(fixture.skill)==fixture.skillBefore,"learning_did_not_grant_skill_points");
            require(!EvaluateUsefulRecipe(InspectUsefulRecipe(*actor,sObjectMgr.GetItemPrototype(fixture.item))).useful,
                "known_recipe_no_longer_a_learning_or_purchase_need");
            actor->SaveToDB();fixture.blocker.clear();emit(true,"",actor);
        } catch (const std::exception& error) {emit(false,error.what());}
    }
}
