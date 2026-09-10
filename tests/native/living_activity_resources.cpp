#include "LivingActivityResources.h"
#include "LivingActivityClaimCodec.h"
#include <cassert>
#include <limits>
#include <stdexcept>
using namespace LivingActivity;
static ResourceClaim ItemClaim(const std::string& suffix, uint32_t guid, uint64_t quantity) {
    ResourceClaim claim;
    claim.id = "ff2efbdf-f0ec-4539-b840-299847970c" + suffix;
    claim.task = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    claim.actor = 497; claim.itemEntry = 2934; claim.itemGuid = guid;
    claim.quantity = quantity; claim.location = "bags"; claim.state = "held";
    return claim;
}
int main() {
    ResourceClaimBook book;
    auto leather = ItemClaim("01", 81, 6);
    assert(ValidResourceClaim(leather));
    assert(book.Protection().UnreservedItem(497,81,2934,10) == 0);
    assert(book.Protection().UnreservedMoney(497,1000) == 0);
    assert(book.InstallReceipt({{leather,0}}) == ClaimInstall::NotReady);
    assert(book.RestoreBatch({leather}) == ClaimInstall::Installed);
    assert(book.RestoreBatch({leather}) == ClaimInstall::Duplicate);
    assert(book.FinishRestore());
    assert(book.Protection().UnreservedItem(497,81,2934,10) == 4);
    // Changing carrier does not make a physical claimed GUID unprotected.
    assert(book.Protection().UnreservedItem(53,81,2934,10) == 4);
    assert(book.Protection().UnreservedItem(497,82,2934,10) == 10);
    assert(book.Protection().UnreservedItem(497,81,2934,3) == 0);
    const auto snapshot = book.Protection();
    auto second = ItemClaim("02",81,3);
    assert(book.InstallReceipt({{second,0}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,81,2934,10) == 1);
    assert(snapshot.UnreservedItem(497,81,2934,10) == 4); // Value snapshot never mutates.
    assert(book.InstallReceipt({{second,0}}) == ClaimInstall::Duplicate);
    auto forged = second; forged.quantity = 1;
    assert(book.InstallReceipt({{forged,0}}) == ClaimInstall::Stale);
    auto released = second; ++released.revision; released.state = "released";
    assert(book.InstallReceipt({{released,1}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,81,2934,10) == 4);
    auto reanimated = released; ++reanimated.revision; reanimated.state = "held";
    assert(book.InstallReceipt({{reanimated,2}}) == ClaimInstall::Invalid);
    auto changedOwner = leather; ++changedOwner.revision; changedOwner.actor = 53;
    assert(book.InstallReceipt({{changedOwner,1}}) == ClaimInstall::Invalid);
    changedOwner = leather; ++changedOwner.revision; changedOwner.itemEntry = 2589;
    assert(book.InstallReceipt({{changedOwner,1}}) == ClaimInstall::Invalid);
    // An acknowledged native stack split/transfer replaces protection in one
    // atomic receipt. This cache cannot itself authorize or perform the split.
    auto split = leather; ++split.revision; split.quantity = 2;
    auto transferred = ItemClaim("03",83,4); transferred.location = "mail";
    transferred.state = "in_transfer"; transferred.nativeReference = 120;
    assert(book.InstallReceipt({{split,1},{transferred,0}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,81,2934,6) == 4);
    assert(book.Protection().UnreservedItem(53,83,2934,4) == 0);
    auto consumed = split; ++consumed.revision; consumed.state = "consumed";
    auto stale = transferred; ++stale.revision;
    assert(book.InstallReceipt({{consumed,2},{stale,0}}) == ClaimInstall::Invalid);
    assert(book.Inspect(split.id)->state == "held"); // No partial batch mutation.
    assert(book.InstallReceipt({{consumed,2},{transferred,0}}) == ClaimInstall::Invalid);
    assert(book.InstallReceipt({{consumed,2},{consumed,2}}) == ClaimInstall::Invalid);
    assert(book.InstallReceipt({{consumed,2}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,81,2934,6) == 6);
    auto unknown = ItemClaim("04",0,3); unknown.state = "reconciling";
    assert(book.InstallReceipt({{unknown,0}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,84,2934,10) == 7);
    assert(book.Protection().UnreservedItem(53,84,2934,10) == 10);
    auto speculative = ItemClaim("05",0,7); speculative.state = "proposed";
    assert(book.InstallReceipt({{speculative,0}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,84,2934,10) == 7);
    auto cancelled = speculative; ++cancelled.revision; cancelled.state = "released";
    assert(book.InstallReceipt({{cancelled,1}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedItem(497,84,2934,10) == 7);
    ResourceClaim money = ItemClaim("06",0,0);
    money.itemEntry = 0; money.copper = 250; money.location = "money";
    assert(book.InstallReceipt({{money,0}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedMoney(497,1000) == 750);
    assert(book.Protection().UnreservedMoney(497,100) == 0);
    auto mailed = money; ++mailed.revision; mailed.location = "mail";
    mailed.nativeReference = 121; mailed.state = "in_transfer";
    assert(book.InstallReceipt({{mailed,1}}) == ClaimInstall::Installed);
    assert(book.Protection().UnreservedMoney(497,750) == 750); // No escrow double-count.
    auto bad = leather; bad.quantity = std::numeric_limits<uint64_t>::max();
    assert(!ValidResourceClaim(bad));
    bad = leather; bad.itemGuid = 0; assert(!ValidResourceClaim(bad));
    bad = leather; bad.location = "mail"; assert(!ValidResourceClaim(bad));
    bad = leather; bad.copper = 1; assert(!ValidResourceClaim(bad));
    bad = money; bad.location = "bank"; assert(!ValidResourceClaim(bad));
    bad = leather; bad.state = "done"; assert(!ValidResourceClaim(bad));
    ResourceClaimBook failed;
    assert(failed.RestoreBatch({bad}) == ClaimInstall::Invalid);
    assert(!failed.FinishRestore());
    assert(failed.Protection().UnreservedMoney(497,1000) == 0);
    ResourceClaimBook bounded(1);
    assert(bounded.RestoreBatch({leather,second}) == ClaimInstall::Capacity);
    assert(bounded.Size() == 0 && !bounded.FinishRestore());
    ResourceClaimBook liveBounded(1); assert(liveBounded.FinishRestore());
    assert(liveBounded.InstallReceipt({{leather,0},{second,0}}) == ClaimInstall::Capacity);
    assert(liveBounded.Size() == 0);
    assert(liveBounded.InstallReceipt({{leather,0}}) == ClaimInstall::Installed);
    assert(liveBounded.InstallReceipt({{second,0}}) == ClaimInstall::Capacity);
    assert(liveBounded.Protection().UnreservedItem(497,81,2934,10) == 4);
    Task task; task.id = task.root = leather.task; task.actor = task.context.actor = leather.actor;
    task.source = "fixture"; task.sourceKey = "native_stock"; task.revision = 2;
    task.createdAtMs = task.updatedAtMs = 1; task.mode = Mode::Active; task.phase = Phase::Preparing;
    NativeResourceBalance balance{497,81,2934,10,0,"bags"};
    const auto receipt = "ff2efbdf-f0ec-4539-b840-299847970c20";
    auto plan = ResourceReservationWrite(task,1,receipt,{{leather,0}},{balance});
    assert(plan.statements.size() == 4);
    assert(plan.statements.front().find("SET actor_guid=actor_guid") != std::string::npos);
    assert(plan.statements.at(1).find("SUM(c.quantity)") != std::string::npos);
    assert(plan.statements.back().find("request_hash=SHA2(") != std::string::npos);
    assert(plan.receiptQuery.find("living_activity_claim") != std::string::npos);
    auto lowerBalance = balance; --lowerBalance.quantity;
    assert(plan.receiptQuery != ResourceReservationWrite(task,1,receipt,{{leather,0}},{lowerBalance}).receiptQuery);
    auto rejects = [&](const std::vector<ClaimReceiptChange>& changes,const std::vector<NativeResourceBalance>& balances) {
        try { ResourceReservationWrite(task,1,receipt,changes,balances); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    assert(rejects({},{}));
    assert(rejects({{leather,0}},{}));
    assert(rejects({{leather,0},{leather,0}},{balance}));
    assert(rejects({{leather,0}},{balance,balance}));
    auto otherActor = balance; ++otherActor.actor;
    assert(rejects({{leather,0}},{otherActor}));
    auto consumption = leather; consumption.state = "consumed";
    assert(rejects({{consumption,0}},{balance}));
    assert(rejects({{transferred,0}},{}));
    task.phase = Phase::Executing;
    assert(rejects({{leather,0}},{balance}));
    const std::string payload = "{\"id\":\"ff2efbdf-f0ec-4539-b840-299847970c01\","
        "\"task\":\"637bd562-36d2-5b01-bc01-e2d831c49f38\",\"actor\":497,\"item_guid\":81,"
        "\"item_entry\":2934,\"quantity\":6,\"copper\":0,\"location\":\"bags\",\"reference\":0,"
        "\"state\":\"held\",\"revision\":1}";
    ResourceClaim decoded; std::string error;
    assert(DecodeClaimProjection(payload,decoded,error));
    assert(SameResourceClaim(decoded,leather));
    for (const auto& invalid : {std::string("{}"),std::string(2049,'x'),payload.substr(0,payload.size()-1)+",\"actor\":497}"}) {
        assert(!DecodeClaimProjection(invalid,decoded,error));
        assert(SameResourceClaim(decoded,leather));
    }
    auto badNumber = payload; badNumber.replace(badNumber.find("497"),3,"-1");
    assert(!DecodeClaimProjection(badNumber,decoded,error));
    badNumber = payload; badNumber.replace(badNumber.find("497"),3,"4294967296");
    assert(!DecodeClaimProjection(badNumber,decoded,error));
}
