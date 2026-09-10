#include "LivingActivitySpellResources.h"
#include "LivingActivityResources.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    assert(CheckUnclaimedReagents(2,{}, {},nullptr) == ReagentReadiness::Ready);
    const std::vector<ReagentNeed> needs={{17030,1}};
    const std::vector<ReagentStack> bags={{20,17030,5},{21,17030,10},{22,2770,4}};
    assert(CheckUnclaimedReagents(2,needs,bags,nullptr) == ReagentReadiness::ProjectionUnavailable);
    ResourceClaimBook claims; assert(claims.FinishRestore());
    auto reader=claims.Reader(); auto clean=reader.Inspect();
    assert(CheckUnclaimedReagents(2,needs,bags,clean.get()) == ReagentReadiness::Ready);
    auto legacy=bags; legacy.front().legacyProtected=true;
    assert(CheckUnclaimedReagents(2,needs,legacy,clean.get()) == ReagentReadiness::ProtectedStack);
    assert(CheckUnclaimedReagents(2,{{2770,4}},legacy,clean.get()) == ReagentReadiness::Ready);
    assert(CheckUnclaimedReagents(2,{{17030,16}},bags,clean.get()) == ReagentReadiness::MissingMaterial);
    assert(CheckUnclaimedReagents(2,{{17030,10},{17030,6}},bags,clean.get()) == ReagentReadiness::MissingMaterial);
    assert(CheckUnclaimedReagents(2,{{17030,0}},bags,clean.get()) == ReagentReadiness::InvalidNativeInput);
    ResourceClaim claim; claim.id="57d484da-2657-4545-ae21-0c4f5dce6881";
    claim.task="57d484da-2657-4545-ae21-0c4f5dce6882"; claim.actor=2;
    claim.itemGuid=20; claim.itemEntry=17030; claim.quantity=1; claim.location="bags"; claim.state="held";
    assert(claims.ReservePending("57d484da-2657-4545-ae21-0c4f5dce6883",{{claim,0}},{{2,20,17030,5,0,"bags"}}) == ClaimInstall::Installed);
    auto pending=reader.Inspect();
    // Fourteen spare reagents cannot make native stack selection safe.
    assert(CheckUnclaimedReagents(2,needs,bags,pending.get()) == ReagentReadiness::ProtectedStack);
    assert(CheckUnclaimedReagents(2,needs,bags,clean.get()) == ReagentReadiness::Ready); // Retained view immutable.
    assert(claims.CommitReservation("57d484da-2657-4545-ae21-0c4f5dce6883") == ClaimInstall::Installed);
    assert(CheckUnclaimedReagents(2,needs,bags,reader.Inspect().get()) == ReagentReadiness::ProtectedStack);
    assert(CheckUnclaimedReagents(2,{{2770,4}},bags,reader.Inspect().get()) == ReagentReadiness::Ready);
    auto repeated=bags; repeated.push_back(bags.front());
    assert(CheckUnclaimedReagents(2,needs,repeated,clean.get()) == ReagentReadiness::InvalidNativeInput);
    auto released=claim; released.state="released"; ++released.revision;
    assert(claims.ReservePending("57d484da-2657-4545-ae21-0c4f5dce6884",{{released,1}}, {}) == ClaimInstall::Installed);
    assert(CheckUnclaimedReagents(2,needs,bags,reader.Inspect().get()) == ReagentReadiness::ProtectedStack);
    assert(claims.CommitReservation("57d484da-2657-4545-ae21-0c4f5dce6884") == ClaimInstall::Installed);
    assert(CheckUnclaimedReagents(2,needs,bags,reader.Inspect().get()) == ReagentReadiness::Ready);
    claims.BlockProjection();
    assert(CheckUnclaimedReagents(2,needs,bags,reader.Inspect().get()) == ReagentReadiness::ProjectionUnavailable);
    assert(CheckUnclaimedReagents(2,{},bags,reader.Inspect().get()) == ReagentReadiness::Ready);
}
