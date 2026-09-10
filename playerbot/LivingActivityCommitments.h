#ifndef LIVING_ACTIVITY_COMMITMENTS_H
#define LIVING_ACTIVITY_COMMITMENTS_H

namespace LivingActivity {
    enum class PartyProtection { None, BotOnly, Human, Unresolved };
    enum class PartyAdmission { SavedExecutor, ValidatedHumanCompatibility, ServiceCompatibility };

    // Called on the world owner using native member slots (including offline
    // members), cached account policy and loaded-player identity. Neither
    // callback may query a database. An absent account is NOT proof of a bot.
    template<class Slots, class BotAccount, class LoadedHuman>
    PartyProtection ReadPartyProtection(const Slots& slots, BotAccount botAccount, LoadedHuman loadedHuman) {
        bool unresolved = slots.empty();
        for (const auto& slot : slots) {
            if (loadedHuman(slot.guid) || (slot.livingActivityAccountId &&
                !botAccount(slot.livingActivityAccountId))) return PartyProtection::Human;
            unresolved |= !slot.livingActivityAccountId;
        }
        return unresolved ? PartyProtection::Unresolved : PartyProtection::BotOnly;
    }
    inline const char* PartyAdmissionBlocker(PartyProtection roster, PartyAdmission admission, bool serviceWindow) {
        if (roster == PartyProtection::None || roster == PartyProtection::BotOnly) return "";
        if (roster == PartyProtection::Unresolved) return "party_roster_unresolved";
        // Saved party executors need their own exact accepted-session/root
        // binding in stage 6. Merely labelling a task Human must not bypass the
        // native party commitment while those producers are not migrated.
        if (admission == PartyAdmission::SavedExecutor) return "human_party_executor_not_migrated";
        if (admission == PartyAdmission::ValidatedHumanCompatibility || serviceWindow) return "";
        return "human_party_commitment";
    }
    inline const char* Name(PartyProtection roster) {
        switch (roster) {
            case PartyProtection::None: return "none";
            case PartyProtection::BotOnly: return "bot_only";
            case PartyProtection::Human: return "human_protected";
            case PartyProtection::Unresolved: return "unresolved_protected";
        }
        return "unresolved_protected";
    }
}
#endif
