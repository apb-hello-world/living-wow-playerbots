#include "LivingActivityClaimConsumption.h"
#include <cassert>
#include <stdexcept>
using namespace LivingActivity;
int main() {
    Task task;
    task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f60";
    task.source="fixture"; task.sourceKey="claimed_operation"; task.actor=task.context.actor=700;
    task.mode=Mode::Active; task.phase=Phase::Verifying; task.revision=4;
    task.createdAtMs=100; task.updatedAtMs=200; task.checkpoint.step="native_fixture";
    ResourceClaim item; item.id="ff2efbdf-f0ec-4539-b840-299847970e01";
    item.task=task.id; item.actor=700; item.itemGuid=500; item.itemEntry=2934;
    item.quantity=5; item.location="bags"; item.state="held";
    auto money=item; money.id="ff2efbdf-f0ec-4539-b840-299847970e02";
    money.quantity=money.itemGuid=money.itemEntry=0; money.copper=30; money.location="money";
    OperationResult result; result.id="ff2efbdf-f0ec-4539-b840-299847970e03";
    result.task=task.id; result.taskRevision=3; result.kind="fixture_consume";
    result.state=OperationState::Verified; result.nativeReference="fixture:proof"; result.evidence="native_fixture_result";
    const std::string receipt="ff2efbdf-f0ec-4539-b840-299847970e04";
    auto plan=ConsumedOperationWrite(task,3,result,receipt,"{}",{{money,30},{item,2}});
    assert(plan.changes.size() == 2);
    assert(plan.changes[0].after.quantity == 3 && plan.changes[0].after.state == "held");
    assert(plan.changes[1].after.copper == 30 && plan.changes[1].after.state == "consumed");
    assert(plan.changes[0].expectedRevision == 1 && plan.changes[0].after.revision == 2);
    auto other=ConsumedOperationWrite(task,3,result,receipt,"{}",{{item,2},{money,30}});
    assert(plan.journal.statements == other.journal.statements && plan.journal.receiptQuery == other.journal.receiptQuery);
    assert(plan.journal.statements.front().find("$.native.claimed_consumption") != std::string::npos);
    assert(plan.journal.statements.back().find("request_hash=") != std::string::npos);
    auto changed=ConsumedOperationWrite(task,3,result,receipt,"{}",{{item,3},{money,30}});
    assert(changed.journal.receiptQuery != plan.journal.receiptQuery);
    auto fails=[&](const std::vector<ClaimConsumption>& claims) {
        bool rejected=false; try {ConsumedOperationWrite(task,3,result,receipt,"{}",claims);}
        catch(const std::invalid_argument&){rejected=true;} assert(rejected);
    };
    fails({}); fails({{item,0}}); fails({{item,6}}); fails({{item,2},{item,1}});
    auto invalid=item; invalid.task="637bd562-36d2-5b01-bc01-e2d831c49f61"; fails({{invalid,1}});
    invalid=item; invalid.location="bank"; fails({{invalid,1}});
    invalid=item; invalid.state="reconciling"; fails({{invalid,1}});
    result.state=OperationState::Reconciling; fails({{item,1}});
    result.state=OperationState::Rejected; fails({{item,1}});
    ResourceClaimBook book; assert(book.RestoreBatch({item,money}) == ClaimInstall::Installed);
    assert(book.FinishRestore());
    const auto before=book.Reader().Inspect();
    assert(book.InstallReceipt(plan.changes) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(700,500,2934,5) == 2);
    assert(book.Protection().UnreservedMoney(700,100) == 100);
    assert(before->UnreservedMoney(700,100) == 70);
}
