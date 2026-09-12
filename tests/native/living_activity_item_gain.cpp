#include "LivingActivityItemGain.h"
#include "LivingActivityStackTransfer.h"
#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
using namespace LivingActivity;
int main() {
    {
        std::string why;
        NativeItemStack source{700,900,3371,5,0,45},empty;
        NativeItemStack target{700,901,3371,7,850,3},merged=target;
        merged.count=12;
        assert(VerifyWholeStackTransfer(source,target,merged,false,why));
        auto moved=source;moved.bagGuid=850;moved.slot=4;
        assert(VerifyWholeStackTransfer(source,empty,moved,true,why));
        assert(!VerifyWholeStackTransfer(source,target,merged,true,why)); // Source must really be gone.
        for (unsigned field=0;field<8;++field) {
            auto bad=merged;
            switch(field) {
            case 0:bad.guid=902;break;
            case 1:bad.actor=701;break;
            case 2:bad.entry=3372;break;
            case 3:bad.count=11;break;
            case 4:bad.count=13;break;
            case 5:bad.bagGuid=851;break;
            case 6:bad.slot=4;break;
            default:bad.guid=source.guid;break;
            }
            assert(!VerifyWholeStackTransfer(source,target,bad,false,why));
        }
        auto bad=empty;bad.count=1;
        assert(!VerifyWholeStackTransfer(source,bad,moved,true,why));
        bad=moved;bad.guid=902;
        assert(!VerifyWholeStackTransfer(source,empty,bad,true,why));
        bad=target;bad.count=UINT32_MAX;
        assert(!VerifyWholeStackTransfer(source,bad,merged,false,why));
        bad=source;bad.count=0;
        assert(!VerifyWholeStackTransfer(bad,target,merged,false,why));
    }
    const ItemGainSpec spec{3371,3};
    NativeItemStack old{700,500,3371,19,0,23};
    auto merged=old; merged.count=20;
    NativeItemStack created{700,501,3371,2,600,0};
    std::vector<VerifiedItemGain> gains;
    std::string blocker;
    assert(VerifyNativeItemGain(700,spec,{old},{merged,created},gains,blocker));
    assert(gains.size()==2 && gains[0].added==1 && gains[1].added==2);
    const auto valid=gains;
    auto fails=[&](ItemGainSpec expected,std::vector<NativeItemStack> before,std::vector<NativeItemStack> after) {
        assert(!VerifyNativeItemGain(700,expected,before,after,gains,blocker));
        assert(gains.empty() && !blocker.empty());
    };
    fails({}, {old}, {merged,created}); fails({3371,0},{old},{merged,created});
    fails({3371,10001},{old},{merged,created}); fails({3371,4},{old},{merged,created});
    fails(spec,{old,old},{merged,created}); fails(spec,{old},{merged,created,created});
    auto changed=created; changed.actor=701; fails(spec,{old},{merged,changed});
    changed=created; changed.entry=3372; fails(spec,{old},{merged,changed});
    changed=created; changed.guid=0; fails(spec,{old},{merged,changed});
    changed=created; changed.count=0; fails(spec,{old},{merged,changed});
    changed=merged; changed.slot=24; fails(spec,{old},{changed,created});
    changed=merged; changed.bagGuid=601; fails(spec,{old},{changed,created});
    changed=merged; changed.guid=502; fails(spec,{old},{changed,created});
    changed=old; changed.count=18; auto larger=created; larger.count=4;
    fails(spec,{old},{changed,larger}); // Net +3 is not proof when an old stack shrank.
    changed=created; changed.bagGuid=0; changed.slot=23; fails(spec,{old},{merged,changed});
    std::vector<NativeItemStack> tooMany;
    for (unsigned i=0;i<16;++i) tooMany.push_back({700,1000+i,3371,1,0,uint8_t(23+i)});
    fails({3371,16},{},tooMany);
    changed=old; changed.count=std::numeric_limits<uint32_t>::max(); fails(spec,{old},{changed});

    Task task; task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f80";
    task.source="fixture"; task.sourceKey="item_gain"; task.actor=task.context.actor=700;
    task.mode=Mode::Active; task.phase=Phase::Verifying; task.revision=4;
    task.createdAtMs=100; task.updatedAtMs=200; task.checkpoint.step="native_fixture";
    OperationResult result; result.id="ff2efbdf-f0ec-4539-b840-299847970f01";
    result.task=task.id; result.taskRevision=3; result.kind="vendor_purchase";
    result.state=OperationState::Verified; result.nativeReference="fixture:metadata"; result.evidence="fixture_only";
    ResourceClaim cash; cash.id="ff2efbdf-f0ec-4539-b840-299847970f02";
    cash.task=task.id; cash.actor=700; cash.copper=30; cash.location="money"; cash.state="held";
    const std::string receipt="ff2efbdf-f0ec-4539-b840-299847970f03";
    const auto claims=ItemGainClaims(task,result.id,spec,valid);
    assert(claims.size()==2 && claims[0].after.quantity==1 && claims[1].after.quantity==2);
    assert(claims[0].after.task==task.root && claims[0].after.state=="held");
    assert(claims[0].after.id==ItemGainClaimId(result.id,500));
    assert(claims[0].after.id!=ItemGainClaimId(receipt,500));
    auto plan=AcquiredOperationWrite(task,3,result,receipt,"{\"money\":70}",{{cash,30}},spec,valid);
    assert(plan.changes.size()==3);
    assert(plan.journal.statements.front().find("$.item_gain")!=std::string::npos);
    assert(plan.journal.receiptQuery.find(SqlValue(claims[1].after.id))!=std::string::npos);
    auto reversed=valid; std::reverse(reversed.begin(),reversed.end());
    const auto repeat=AcquiredOperationWrite(task,3,result,receipt,"{\"money\":70}",{{cash,30}},spec,reversed);
    assert(plan.journal.statements==repeat.journal.statements && plan.journal.receiptQuery==repeat.journal.receiptQuery);
    ResourceClaimBook book(18); assert(book.RestoreBatch({cash})==ClaimInstall::Installed); assert(book.FinishRestore());
    assert(book.CanAdmitNewClaims(15));
    assert(book.ReservePending(receipt,claims,{{700,500,3371,20,0,"bags"},{700,501,3371,2,0,"bags"}})==ClaimInstall::Installed);
    assert(!book.CanAdmitNewClaims(16));
    assert(book.Reader().Inspect()->UnreservedItem(700,501,3371,2)==0);
    assert(book.CommitReservation(receipt)==ClaimInstall::Installed);
    assert(book.InstallReceipt({plan.changes.front()})==ClaimInstall::Installed);
    assert(book.Protection().UnreservedMoney(700,70)==70);
    assert(book.Protection().UnreservedItem(700,500,3371,20)==19);
    assert(book.Protection().UnreservedItem(700,501,3371,2)==0);
    result.state=OperationState::Reconciling;
    bool rejected=false;
    try { AcquiredOperationWrite(task,3,result,receipt,"{}",{{cash,30}},spec,valid); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
}
