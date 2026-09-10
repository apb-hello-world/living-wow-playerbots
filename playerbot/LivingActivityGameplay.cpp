#include "playerbot/playerbot.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityCoordinator.h"
#include "ServerFacade.h"
#include "Entities/Pet.h"
#include "Groups/Group.h"

namespace LivingActivity {
    namespace {
        bool NativePartyEngaged(Player& actor, Unit& enemy) {
            if (!actor.IsInWorld() || !enemy.IsInWorld() || actor.GetMapId() != enemy.GetMapId() ||
                actor.GetInstanceId() != enemy.GetInstanceId()) return false;
            // Same native victim/attacker/threat evidence as party positioning.
            // Merely being 'in combat' does not authorize pulling a new enemy.
            auto engaged = [&](Unit* member) {
                return member && member->IsInWorld() && member->GetMapId() == enemy.GetMapId() &&
                    member->GetInstanceId() == enemy.GetInstanceId() &&
                    (enemy.GetVictim() == member || member->getAttackers().count(&enemy) ||
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
    }
    Effects NativeSpellEffects(PlayerbotAI& ai, uint32_t spell, bool itemCast) {
        const auto* info = spell ? sServerFacade.LookupSpellInfo(spell) : nullptr;
        const bool freeHeal = info && ai.GetBot() && InventoryFreeDirectHeal(*info, ai.GetBot()->HasSpell(spell),
            itemCast, SPELL_EFFECT_HEAL, SPELL_EFFECT_HEAL_MAX_HEALTH);
        return {SpellEffectMask(freeHeal), Lane::Managed, true};
    }
    NativePermit NativeSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target, Item* item) {
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
        // Ordinary native cast checks retain reagent, mana, range, cooldown,
        // stance and target requirements. They do not execute the spell.
        if (!ai.CanCastSpell(spell, target, uint8((1u << MAX_EFFECT_INDEX) - 1), true, item)) return {};
        permit.lane = healing ? Lane::Healing : Lane::Combat;
        permit.validated = permit.world.actor == actor->GetGUIDLow();
        return permit;
    }
}
