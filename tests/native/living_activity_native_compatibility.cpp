#include "LivingActivityScope.h"
#include "LivingActivityResources.h"
#include <cassert>
#include <thread>
using namespace LivingActivity;

int main() {
    const uint32_t actor=497;
    const char* rootId="637bd562-36d2-5b01-bc01-e2d831c49f38";
    const char* receipt="ff2efbdf-f0ec-4539-b840-299847970c00";
    WorldContext world;world.actor=actor;world.boot="9411eb6c-d355-4618-b323-1c8e0b0daaa2";
    world.actorGeneration=world.mapGeneration=world.policyRevision=1;
    ExecutionAuthority authority;PermissionPublisher publisher;const auto reader=publisher.Reader();
    ResourceClaimBook claims;
    auto publish=[&]{publisher.Publish(authority.Read(actor));};
    assert(authority.Observe(world,0).code==AuthorityCode::Allowed);publish();
    auto check=[&](Effects effects,uint32_t safety=0) {
        const auto protection=claims.Reader().Inspect();
        return ExecutionScope::Check(reader,effects,world,200,safety,
            protection ? protection->NativeBlockedEffects(actor) : AllEffects);
    };
    const Effects travel{Mask(Effect::Movement)|Mask(Effect::TravelTarget),Lane::Managed,true};
    const Effects inventory{Mask(Effect::Inventory),Lane::Managed,true};
    // No missing snapshot/restore result is interpreted as an empty claim book.
    assert(check({})==AuthorityCode::UnknownAction);
    assert(claims.FinishRestore());
    assert(check({})==AuthorityCode::Allowed);
    assert(check(travel)==AuthorityCode::Allowed && check(inventory)==AuthorityCode::Allowed);
    assert(check({Mask(Effect::Inventory),Lane::Inspection,true})==AuthorityCode::EffectsDenied);
    assert(check({1024,Lane::Managed,true})==AuthorityCode::EffectsDenied);
    // Ordinary safety/reaction behavior remains the native engine's decision
    // when no commitment conflicts. This grants no managed/native permit.
    assert(check({},uint32_t(Safety::Death))==AuthorityCode::Allowed);
    assert(check({},uint32_t(Safety::Combat))==AuthorityCode::Allowed);
    assert(!ExecutionScope::OwnsNativeOperation(actor));
    {
        EvaluationScope evaluation(true);
        assert(check(travel)==AuthorityCode::EffectsDenied && evaluation.Rejected());
    }
    // A foreign/stale attribution cannot launder itself into the native path.
    Task task;task.id=task.root=rootId;task.actor=actor;task.context=world;
    task.source="profession";task.sourceKey=rootId;task.mode=Mode::Active;
    task.phase=Phase::Traveling;task.createdAtMs=task.updatedAtMs=1;
    ActionContext action;action.task=action.rootTask=rootId;action.world=world;
    action.revision=task.revision;action.origin="native_test";action.permittedEffects=travel.mask;
    {
        auto foreign=task;foreign.actor=498;ExecutionScope wrong(foreign,action);
        assert(check(travel)==AuthorityCode::StaleLease);
    }
    {
        ExecutionScope stale(task,action);assert(check(travel)==AuthorityCode::StaleLease);
    }
    const auto lease=authority.Acquire(task,travel.mask,100,1000);
    assert(lease.Granted());publish();task.ownerGeneration=action.ownerGeneration=lease.lease.generation;
    assert(check({})==AuthorityCode::UnknownAction && check(travel)==AuthorityCode::StaleLease);
    {
        ExecutionScope owned(task,action);assert(check(travel)==AuthorityCode::Allowed);
    }
    // Actual reviewed native combat and recovery paths remain available under
    // an owner, but cannot replace its travel target or impersonate its job.
    NativePermit native{world,Lane::Combat,Mask(Effect::Movement)|Mask(Effect::Spell),uint32_t(Safety::Combat),true};
    {
        ExecutionScope combat(native);
        assert(check({native.effects,Lane::Combat,true},uint32_t(Safety::Combat))==AuthorityCode::Allowed);
        assert(check(travel)==AuthorityCode::StaleLease);
    }
    native.lane=Lane::Safety;native.allowedSafety=uint32_t(Safety::Death);
    {
        ExecutionScope recovery(native);
        assert(check({native.effects,Lane::Safety,true},uint32_t(Safety::Death))==AuthorityCode::Allowed);
    }
    assert(ExecutionAuthority::Check(authority.Read(actor),travel,world,2000,nullptr,nullptr,nullptr,0,0)==AuthorityCode::StaleLease);
    assert(authority.Release(lease.lease).code==AuthorityCode::Released);publish();
    assert(check(travel)==AuthorityCode::Allowed);
    {
        ExecutionScope stale(task,action);assert(check(travel)==AuthorityCode::StaleLease);
    }
    // Accepted materials survive loss/release of movement ownership. Pending
    // reservation protection exists before its asynchronous receipt commits.
    ResourceClaim item;item.id="ff2efbdf-f0ec-4539-b840-299847970c01";
    item.task=rootId;item.actor=actor;item.itemGuid=10800;item.itemEntry=2934;
    item.quantity=2;item.location="bags";item.state="held";
    assert(claims.ReservePending(receipt,{{item,0}},{{actor,10800,2934,5,0,"bags"}})==ClaimInstall::Installed);
    const auto pending=claims.Reader().Inspect();
    assert(pending->NativeBlockedEffects(actor) && !pending->NativeBlockedEffects(498));
    assert(check({})==AuthorityCode::UnknownAction && check(inventory)!=AuthorityCode::Allowed);
    assert(check({Mask(Effect::Spell),Lane::Managed,true})!=AuthorityCode::Allowed);
    assert(check(travel)==AuthorityCode::Allowed);
    assert(pending->UnreservedItem(actor,10800,2934,5)==3);
    assert(claims.CommitReservation(receipt)==ClaimInstall::Installed);
    auto banked=item;++banked.revision;banked.location="bank";
    assert(claims.InstallReceipt({{banked,1}})==ClaimInstall::Installed);
    assert(check(inventory)!=AuthorityCode::Allowed);
    ResourceClaim money=item;money.id="ff2efbdf-f0ec-4539-b840-299847970c02";
    money.itemGuid=money.itemEntry=0;money.quantity=0;money.copper=30;money.location="money";
    assert(claims.InstallReceipt({{money,0}})==ClaimInstall::Installed);
    auto released=banked;++released.revision;released.state="released";
    assert(claims.InstallReceipt({{released,2}})==ClaimInstall::Installed);
    assert(check(inventory)!=AuthorityCode::Allowed); // Money protection also survives.
    assert(claims.Reader().Inspect()->UnreservedMoney(actor,100)==70);
    ++money.revision;money.state="released";
    assert(claims.InstallReceipt({{money,1}})==ClaimInstall::Installed);
    assert(check({})==AuthorityCode::Allowed);
    assert(pending->NativeBlockedEffects(actor)); // Prior immutable view unchanged.
    // Reproduce a native map/session transition between durable task steps:
    // no item claims, but the acknowledged activity still owns route intent.
    const auto committed=ExecutionAuthority::NativeCommitmentEffects(task);
    assert(committed==(travel.mask|Mask(Effect::Group)));
    assert(authority.SetCommitmentEffects(actor,committed));publish();
    assert(check(travel)==AuthorityCode::StaleLease && check({})==AuthorityCode::UnknownAction);
    assert(check(inventory)==AuthorityCode::Allowed); // Resource protection is separate.
    auto oldWorld=world;++world.mapGeneration;
    authority.Observe(world,0);publish();
    assert(authority.Read(actor).commitmentEffects==committed);
    assert(check(travel)==AuthorityCode::StaleLease);
    assert(ExecutionScope::Check(reader,travel,oldWorld,200,0,0)==AuthorityCode::StaleContext);
    for(auto lane:{Lane::Combat,Lane::Healing,Lane::Loot,Lane::Safety}) {
        NativePermit valid{world,lane,Mask(Effect::Movement),0,true};
        ExecutionScope exception(valid);
        assert(check({valid.effects,lane,true})==AuthorityCode::Allowed);
        assert(check(travel)==AuthorityCode::StaleLease);
    }
    task.context=world;
    auto renewed=authority.Acquire(task,travel.mask,100,1000);assert(renewed.Granted());
    action.world=world;action.ownerGeneration=renewed.lease.generation;
    task.ownerGeneration=renewed.lease.generation;publish();
    {ExecutionScope resumed(task,action);assert(check(travel)==AuthorityCode::Allowed);}
    assert(authority.Release(renewed.lease).code==AuthorityCode::Released);publish();
    assert(check(travel)==AuthorityCode::StaleLease); // Lease release is not cancellation.
    auto human=task;human.id=human.root=receipt;human.priority=Priority::Human;
    auto humanLease=authority.Acquire(human,travel.mask,100,1000);assert(humanLease.Granted());
    auto humanAction=action;humanAction.task=humanAction.rootTask=receipt;
    humanAction.ownerGeneration=humanLease.lease.generation;publish();
    {ExecutionScope acceptedHuman(human,humanAction);assert(check(travel)==AuthorityCode::Allowed);}
    assert(authority.Release(humanLease.lease).code==AuthorityCode::Released);
    for(auto phase:{Phase::Preparing,Phase::Traveling,Phase::Executing,Phase::Verifying,Phase::Paused,Phase::Reconciling}) {
        auto saved=task;saved.phase=phase;
        assert(ExecutionAuthority::NativeCommitmentEffects(saved)==committed);
        saved.accepted=false;assert(!ExecutionAuthority::NativeCommitmentEffects(saved));
        saved.accepted=true;saved.mode=Mode::Observe;assert(!ExecutionAuthority::NativeCommitmentEffects(saved));
    }
    for(auto phase:{Phase::Queued,Phase::WaitingExternal,Phase::Deferred,Phase::Completed,Phase::Cancelled,Phase::Failed}) {
        auto saved=task;saved.phase=phase;
        assert(!ExecutionAuthority::NativeCommitmentEffects(saved));
    }
    assert(!authority.SetCommitmentEffects(actor,1024));
    assert(authority.Read(actor).commitmentEffects==committed);
    assert(authority.SetCommitmentEffects(actor,0));publish();
    assert(check(travel)==AuthorityCode::Allowed); // Acknowledged wait/closure frees it.
    // A physical handoff protects the recipient, not only the former owner.
    // An unresolved item identity and money already in mail escrow still count.
    auto parcel=item;parcel.id="ff2efbdf-f0ec-4539-b840-299847970c04";
    assert(claims.InstallReceipt({{parcel,0}})==ClaimInstall::Installed);
    auto sent=parcel;sent.state="consumed";++sent.revision;
    parcel.id="ff2efbdf-f0ec-4539-b840-299847970c06";
    parcel.actor=498;parcel.location="mail";parcel.nativeReference=100;
    assert(claims.InstallReceipt({{sent,1},{parcel,0}})==ClaimInstall::Installed);
    assert(!claims.Reader().Inspect()->NativeBlockedEffects(actor));
    assert(claims.Reader().Inspect()->NativeBlockedEffects(498));
    assert(claims.Reader().Inspect()->UnreservedItem(actor,10800,2934,5)==3); // GUID is global.
    auto unknown=item;unknown.id="ff2efbdf-f0ec-4539-b840-299847970c03";
    unknown.itemGuid=0;unknown.state="reconciling";
    assert(claims.InstallReceipt({{unknown,0}})==ClaimInstall::Installed);
    assert(check({})==AuthorityCode::UnknownAction);
    ++unknown.revision;unknown.state="released";
    assert(claims.InstallReceipt({{unknown,1}})==ClaimInstall::Installed);
    money.id="ff2efbdf-f0ec-4539-b840-299847970c05";money.revision=1;
    money.state="held";money.location="mail";money.nativeReference=101;
    assert(claims.InstallReceipt({{money,0}})==ClaimInstall::Installed);
    assert(claims.Reader().Inspect()->UnreservedMoney(actor,100)==100); // Escrow is not wallet money.
    assert(check({})==AuthorityCode::UnknownAction); // It is still an obligation.
    // Context revocation and actor/map changes remain fail-closed.
    auto staleWorld=world;++staleWorld.mapGeneration;
    assert(ExecutionScope::Check(reader,travel,staleWorld,200,0,0)==AuthorityCode::StaleContext);
    publisher.Revoke();assert(check(travel)==AuthorityCode::NoOwner);
    publish();claims.BlockProjection();assert(check({})==AuthorityCode::UnknownAction);
    std::thread worker([&]{assert(check({})==AuthorityCode::UnknownAction);});worker.join();
}
