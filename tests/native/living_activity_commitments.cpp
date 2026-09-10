#include "LivingActivityCommitments.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using namespace LivingActivity;
struct NativeMemberSlot { uint32_t guid, livingActivityAccountId; };
int main() {
    // Uses the same member-slot contract as native Group, not online-only
    // references. Offline humans remain protected without account queries.
    std::vector<NativeMemberSlot> roster{{2, 100}, {3, 101}};
    auto botAccount = [](uint32_t account) { return account >= 100; };
    auto offline = [](uint32_t) { return false; };
    assert(ReadPartyProtection(roster, botAccount, offline) == PartyProtection::BotOnly);
    roster.push_back({4, 20});
    assert(ReadPartyProtection(roster, botAccount, offline) == PartyProtection::Human);
    for (auto admission : {PartyAdmission::SavedExecutor, PartyAdmission::ServiceCompatibility})
        assert(*PartyAdmissionBlocker(PartyProtection::Human, admission, false));
    assert(!*PartyAdmissionBlocker(PartyProtection::Human, PartyAdmission::ValidatedHumanCompatibility, false));
    assert(!*PartyAdmissionBlocker(PartyProtection::Human, PartyAdmission::ServiceCompatibility, true));
    // A task's Human priority or a claimed free-time window cannot impersonate
    // a validated party executor. Its root/session producer is not migrated.
    assert(std::string(PartyAdmissionBlocker(PartyProtection::Human, PartyAdmission::SavedExecutor, true)) ==
        "human_party_executor_not_migrated");
    roster.back().livingActivityAccountId = 0;
    assert(ReadPartyProtection(roster, botAccount, offline) == PartyProtection::Unresolved);
    for (auto admission : {PartyAdmission::SavedExecutor, PartyAdmission::ServiceCompatibility,
                          PartyAdmission::ValidatedHumanCompatibility})
        assert(std::string(PartyAdmissionBlocker(PartyProtection::Unresolved, admission, true)) == "party_roster_unresolved");
    roster.pop_back();
    assert(ReadPartyProtection(roster, botAccount, [](uint32_t id) { return id == 3; }) == PartyProtection::Human);
    for (auto protection : {PartyProtection::None, PartyProtection::BotOnly})
        for (auto admission : {PartyAdmission::SavedExecutor, PartyAdmission::ServiceCompatibility,
                              PartyAdmission::ValidatedHumanCompatibility})
            assert(!*PartyAdmissionBlocker(protection, admission, false));
    roster.clear();
    assert(ReadPartyProtection(roster, botAccount, offline) == PartyProtection::Unresolved);
}
