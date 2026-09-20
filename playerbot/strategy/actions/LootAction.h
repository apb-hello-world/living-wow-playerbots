#pragma once
#include "playerbot/LootObjectStack.h"
#include "MovementActions.h"

namespace ai
{
    class LootAction : public MovementAction
    {
    public:
        LootAction(PlayerbotAI* ai) : MovementAction(ai, "loot") {}
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override;
        virtual bool Execute(Event& event) override;
    };

    class OpenLootAction : public MovementAction
    {
    public:
        OpenLootAction(PlayerbotAI* ai) : MovementAction(ai, "open loot") {}
        LivingActivity::Effects GetActivityEffects() const override;
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override;
        virtual bool Execute(Event& event) override;

    private:
        bool DoLoot(LootObject& lootObject);
        uint32 GetOpeningSpell(LootObject& lootObject);
        uint32 GetOpeningSpell(LootObject& lootObject, GameObject* go);
        bool CanOpenLock(LootObject& lootObject, const SpellEntry* pSpellInfo, GameObject* go);
        bool CanOpenLock(uint32 skillId, uint32 reqSkillValue);
    };

    class StoreLootAction : public Action
    {
    public:
        StoreLootAction(PlayerbotAI* ai) : Action(ai, "store loot") {}
        LivingActivity::Effects GetActivityEffects() const override { return {
            LivingActivity::Mask(LivingActivity::Effect::Inventory)|LivingActivity::Mask(LivingActivity::Effect::Money),
            LivingActivity::Lane::Managed,true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override;
        virtual bool Execute(Event& event) override;
        static bool IsLootAllowed(ItemQualifier& itemQualifier, PlayerbotAI *ai);
    };

    class ReleaseLootAction : public MovementAction
    {
    public:
        ReleaseLootAction(PlayerbotAI* ai) : MovementAction(ai, "release loot") {}
        virtual bool Execute(Event& event) override;
    };
}
