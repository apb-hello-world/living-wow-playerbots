#include "playerbot/playerbot.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityCoordinator.h"
#include "ServerFacade.h"
#include "Entities/Pet.h"
#include "Groups/Group.h"
#include "LivingActivitySpellResources.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"

namespace LivingActivity {
    namespace {
        bool NativePartyEngaged(Player& actor, Unit& enemy) {
            if (!actor.IsInWorld() || !enemy.IsInWorld() || actor.GetMapId() != enemy.GetMapId() ||
                actor.GetInstanceId() != enemy.GetInstanceId()) return false;
            // Same native victim/attacker/threat evidence as party positioning.
            // Merely being 'in combat' does not authorize pulling a new enemy.
            auto engaged = [&](Unit* member) {
                return member && member->IsInWorld() && member->IsAlive() && member->GetMapId() == enemy.GetMapId() &&
                    member->GetInstanceId() == enemy.GetInstanceId() &&
                    (enemy.GetVictim() == member || member->GetVictim() == &enemy || member->getAttackers().count(&enemy) ||
                        enemy.getThreatManager().HasThreat(member, true));
            };
            if (engaged(&actor) || engaged(actor.GetPet())) return true;
            if (auto* group = actor.GetGroup()) {
                unsigned inspected = 0;
                for (auto* ref = group->GetFirstMember(); ref && inspected++ < 40; ref = ref->next())
                    if (auto* member = ref->getSource())
                        if (engaged(member) || engaged(member->GetPet())) return true;
            }
            return false;
        }
        bool NativeMemberEngaged(Player& actor, Unit& member) {
            auto validEnemy = [&](Unit* enemy) {
                return enemy && enemy->IsInWorld() && enemy->IsAlive() &&
                    sServerFacade.IsHostileTo(&actor, enemy) && NativePartyEngaged(actor, *enemy);
            };
            if (validEnemy(member.GetVictim())) return true;
            unsigned inspected = 0;
            for (auto* enemy : member.getAttackers()) {
                if (inspected++ >= 32) break;
                if (validEnemy(enemy)) return true;
            }
            return false;
        }
    }
    NativePermit NativeCombatMovementPermit(PlayerbotAI& ai, Unit* target) {
        auto permit = sLivingActivityCoordinator.NativeActionContext(ai, Lane::Combat,
            Mask(Effect::Movement), uint32_t(Safety::Combat));
        if (!permit.world.actor) return {};
        auto* actor = ai.GetBot();
        if (!actor || !target || !actor->IsInWorld() || !target->IsInWorld() ||
            actor->GetMapId() != target->GetMapId() || actor->GetInstanceId() != target->GetInstanceId()) return {};
        if (target->GetTypeId() == TYPEID_PLAYER && static_cast<Player*>(target)->IsBeingTeleported()) return {};
        const bool hostile = sServerFacade.IsHostileTo(actor, target);
        const bool ally = target != actor && target->GetTypeId() == TYPEID_PLAYER && actor->GetGroup() &&
            static_cast<Player*>(target)->GetGroup() == actor->GetGroup() && sServerFacade.IsFriendlyTo(actor, target);
        const bool engaged = hostile ? NativePartyEngaged(*actor, *target) :
            (ally && (NativeMemberEngaged(*actor, *actor) || NativeMemberEngaged(*actor, *target)));
        permit.validated = ReadyForNativeCombatMovement(*actor, *target, hostile, ally, engaged);
        return permit.validated ? permit : NativePermit{};
    }
    Effects NativeSpellEffects(PlayerbotAI& ai, uint32_t spell, bool itemCast) {
        const auto* info = spell ? sServerFacade.LookupSpellInfo(spell) : nullptr;
        auto* actor = ai.GetBot();
        const bool known = actor && (actor->HasSpell(spell) || (actor->GetPet() && actor->GetPet()->HasSpell(spell)));
        const bool freeHeal = info && actor && InventoryFreeDirectHeal(*info, known,
            itemCast, SPELL_EFFECT_HEAL, SPELL_EFFECT_HEAL_MAX_HEALTH);
        return {SpellEffectMask(freeHeal), Lane::Managed, true};
    }
    NativePermit NativeEngagedAttackPermit(PlayerbotAI& ai, Unit* target) {
        auto permit = sLivingActivityCoordinator.NativeActionContext(ai, Lane::Combat,
            AttackEffectMask(), uint32_t(Safety::Combat));
        if (!permit.world.actor) return {};
        auto* actor = ai.GetBot();
        if (!actor || !target || (target->GetTypeId() == TYPEID_PLAYER &&
            static_cast<Player*>(target)->IsBeingTeleported())) return {};
        permit.validated = ReadyForNativeEngagedAttack(*actor, *target,
            sServerFacade.IsHostileTo(actor, target), NativePartyEngaged(*actor, *target),
            sServerFacade.GetDistance2d(actor, target) <= sPlayerbotAIConfig.sightDistance);
        return permit.validated ? permit : NativePermit{};
    }
    NativePermit NativeSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target, Item* item) {
        if (!item && ai.GetBot() && ai.GetBot()->GetPet() && ai.GetBot()->GetPet()->HasSpell(spell))
            return NativePetSpellPermit(ai, spell, target);
        const auto effects = NativeSpellEffects(ai, spell, item != nullptr);
        auto permit = sLivingActivityCoordinator.NativeActionContext(ai, Lane::Healing,
            effects.mask, uint32_t(Safety::Combat));
        if (!permit.world.actor) return {};
        Player* actor = ai.GetBot();
        const auto* info = spell ? sServerFacade.LookupSpellInfo(spell) : nullptr;
        if (!actor || !target || !info) return {};
        const bool known = actor->HasSpell(spell);
        const bool healing = ReadyForNativeHealing(*actor, *target, known,
            PlayerbotAI::IsHealSpell(info), sServerFacade.IsFriendlyTo(actor, target));
        if (!healing && !ReadyForNativeOffense(*actor, *target, known, IsPositiveSpell(info),
            sServerFacade.IsHostileTo(actor, target), NativePartyEngaged(*actor, *target))) return {};
        // Combat/healing is not permission to spend another obligation's stock.
        // Read only native bag identities plus the immutable acknowledged/pending
        // claim projection. Empty reagent lists avoid any bag scan or DB work.
        std::vector<ReagentNeed> reagents;
        if (!actor->CanNoReagentCast(info))
            for (uint32_t i=0;i<MAX_SPELL_REAGENTS;++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0)
                    reagents.push_back({uint32_t(info->Reagent[i]),uint32_t(info->ReagentCount[i])});
        if (!reagents.empty()) {
            std::vector<ReagentStack> bags;
            const auto trade=sPlayerbotActionBroker.ReservedItemsView();
            const auto supply=sGuildSupplies.ReservedItemsView();
            for (auto* stack : ai.InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
                if (stack) bags.push_back({stack->GetGUIDLow(),stack->GetEntry(),stack->GetCount(),
                    trade->Item(stack->GetGUIDLow()) || supply->Item(stack->GetGUIDLow()) ||
                    supply->Entry(actor->GetGUIDLow(),stack->GetEntry())});
            const auto protection=sLivingActivityCoordinator.ResourceReservations().Inspect();
            if (CheckUnclaimedReagents(actor->GetGUIDLow(),reagents,bags,protection.get()) != ReagentReadiness::Ready)
                return {};
        }
        // Ordinary native cast checks retain reagent, mana, range, cooldown,
        // stance and target requirements. They do not execute the spell.
        if (!ai.CanCastSpell(spell, target, uint8((1u << MAX_EFFECT_INDEX) - 1), true, item)) return {};
        permit.lane = healing ? Lane::Healing : Lane::Combat;
        permit.validated = permit.world.actor == actor->GetGUIDLow();
        return permit;
    }
    NativePermit NativePetSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target) {
        auto permit = sLivingActivityCoordinator.NativeActionContext(ai, Lane::Combat,
            NativeSpellEffects(ai, spell).mask, uint32_t(Safety::Combat));
        if (!permit.world.actor) return {};
        auto* actor = ai.GetBot();
        auto* pet = actor ? actor->GetPet() : nullptr;
        const auto* info = spell ? sServerFacade.LookupSpellInfo(spell) : nullptr;
        if (!actor || !pet || !info || IsPassiveSpell(info) ||
            !ReadyForNativePetCaster(*actor, *pet, pet->HasSpell(spell),
                pet->GetOwnerGuid() == actor->GetObjectGuid(), pet->IsSpellReady(*info))) return {};
        if (!target && IsPositiveSpell(info)) target = pet;
        if (!target || (target->GetTypeId() == TYPEID_PLAYER &&
            static_cast<Player*>(target)->IsBeingTeleported())) return {};
        const bool healing = ReadyForNativeHealing(*actor, *target, true,
            PlayerbotAI::IsHealSpell(info), sServerFacade.IsFriendlyTo(pet, target));
        const bool offense = ReadyForNativeOffense(*actor, *target, true, IsPositiveSpell(info),
            sServerFacade.IsHostileTo(pet, target), NativePartyEngaged(*actor, *target));
        if (!healing && !offense) return {};
        if (sServerFacade.GetDistance2d(actor, target) > sPlayerbotAIConfig.sightDistance) return {};
        // No owner-stock exception for a pet ability. Such a native requirement
        // needs an explicit resource adapter, not the owner's spellbook check.
        for (const auto reagent : info->Reagent) if (reagent > 0) return {};
        // PlayerbotAI::CanCastSpell has a pet shortcut which checks only known
        // spell/cooldown. Inspect an actual PET cast instead. CheckCast performs
        // no execution; native HandlePetAction remains the only command path.
        Spell nativeCast(pet, info, TRIGGERED_NORMAL_COMBAT_CAST | TRIGGERED_PET_CAST);
        nativeCast.m_targets.setUnitTarget(IsSpellRequireTarget(info) ? target : nullptr);
        const auto result = nativeCast.CheckCast(true);
        switch (result) {
            case SPELL_CAST_OK:
            case SPELL_FAILED_NOT_INFRONT:
            case SPELL_FAILED_UNIT_NOT_INFRONT:
            case SPELL_FAILED_NOT_STANDING:
            case SPELL_FAILED_MOVING:
                break;
            case SPELL_FAILED_OUT_OF_RANGE:
            case SPELL_FAILED_LINE_OF_SIGHT:
                // The native pet handler may path to an existing enemy before
                // casting. This cannot grant permission to pull a new target.
                if (!offense || !pet->CanAttackNow(target)) return {};
                break;
            default: return {};
        }
        permit.lane = healing ? Lane::Healing : Lane::Combat;
        permit.validated = permit.world.actor == actor->GetGUIDLow();
        return permit;
    }
}
