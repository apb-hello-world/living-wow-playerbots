#include "LivingCommissionMeeting.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;

Task order() {
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={"lwc-123","meeting-order","meeting",EncodeProfessionJob(recipe),20,28,60,1000};
    job.craft=job.agreement.recipe;job.craftFinishedRevision=5;
    Task t;t.id=t.root="637bd562-36d2-5b01-bc01-e2d831c49f92";t.actor=t.context.actor=20;
    t.kind=Kind::Commission;t.source="commission_job";t.sourceKey=job.agreement.id;t.revision=5;
    t.mode=Mode::Active;t.phase=Phase::Preparing;t.createdAtMs=t.updatedAtMs=1000;
    t.checkpoint.step="commission_craft_ready";t.checkpoint.data=EncodeCommissionJob(job);
    return t;
}
int main() {
    auto t=order();Task next;std::string why;
    CommissionMeetingObservation o;o.recipient=28;o.x=100;o.distance=100;
    assert(PrepareCommissionMeeting(t,o,2000,next,why) && next.phase==Phase::Traveling);
    CommissionJob job;assert(DecodeCommissionJob(next.checkpoint.data,job,why) && job.meeting);
    assert(job.agreement.feeCopper==60 && job.agreement.recipient==28 && job.craftFinishedRevision==5);
    t=next;
    assert(!PrepareCommissionMeeting(t,o,2001,next,why) && why=="commission_meeting_traveling");
    assert(PrepareCommissionMeeting(t,o,32000,next,why) && next.phase==Phase::Traveling);
    assert(DecodeCommissionJob(next.checkpoint.data,job,why) && job.meeting->noProgressMs==30000);
    t=next;o.distance=90;
    assert(PrepareCommissionMeeting(t,o,62000,next,why));
    assert(DecodeCommissionJob(next.checkpoint.data,job,why) && job.meeting->noProgressMs==0 && job.meeting->bestDistance==90);
    t=next;
    // Going in circles cannot keep resetting the no-progress clock.
    for(int i=0;i<4;++i) {
        o.distance=i%2?90:93;assert(PrepareCommissionMeeting(t,o,t.updatedAtMs+30000,next,why));t=next;
    }
    assert(t.phase==Phase::Deferred && t.retryAtMs==t.updatedAtMs+5000);
    assert(DecodeCommissionJob(t.checkpoint.data,job,why) && job.meeting->attempts==1);
    assert(!PrepareCommissionMeeting(t,o,t.retryAtMs-1,next,why));
    const auto elapsed=t.checkpoint.activeElapsedMs;
    assert(PrepareCommissionMeeting(t,o,t.retryAtMs,next,why) && next.phase==Phase::Reconciling);t=next;
    assert(PrepareCommissionMeeting(t,o,t.updatedAtMs+1,next,why) && next.phase==Phase::Preparing);t=next;
    assert(PrepareCommissionMeeting(t,o,t.updatedAtMs+1,next,why) && next.phase==Phase::Traveling);t=next;
    assert(t.checkpoint.activeElapsedMs==elapsed);
    for(int i=0;i<4;++i) {assert(PrepareCommissionMeeting(t,o,t.updatedAtMs+30000,next,why));t=next;}
    assert(t.phase==Phase::Deferred && t.retryAtMs==t.updatedAtMs+300000);
    auto restarted=AfterRestart(t,t.updatedAtMs+1000);
    assert(restarted.retryAtMs==t.retryAtMs && restarted.checkpoint.data==t.checkpoint.data);
    assert(!PrepareCommissionMeeting(restarted,o,restarted.updatedAtMs,next,why));
    // Offline and safety waiting preserve the agreement, claims and active time.
    t=order();o.blocker="commission_meeting_recipient_offline";
    assert(PrepareCommissionMeeting(t,o,2000,next,why) && next.phase==Phase::WaitingExternal);t=next;
    assert(PrepareCommissionMeeting(t,o,100000,next,why) && next.checkpoint.activeElapsedMs==0);
    o.blocker="transport_active";o.safety=true;
    assert(PrepareCommissionMeeting(t,o,100000,next,why) && next.phase==Phase::Paused);
    o={};o.recipient=28;o.arrived=true;t=order();
    assert(PrepareCommissionMeeting(t,o,2000,next,why) && next.phase==Phase::Preparing &&
        next.checkpoint.step=="commission_trade_prepare");
    assert(next.phase!=Phase::Completed); // Neither elapsed time nor assembly is a payment receipt.
    for(int bad=0;bad<8;++bad) {
        auto invalid=t;auto observation=o;
        if(bad==0)observation.recipient=29;
        if(bad==1)invalid.accepted=false;
        if(bad==2)invalid.mode=Mode::Observe;
        if(bad==3)invalid.phase=Phase::Executing;
        if(bad==4)invalid.phase=Phase::Cancelled;
        if(bad==5)invalid.checkpoint.step="commission_trade_offer";
        if(bad==6) {CommissionJob j;assert(DecodeCommissionJob(invalid.checkpoint.data,j,why));j.agreement.delivery="direct";invalid.checkpoint.data=EncodeCommissionJob(j);}
        if(bad==7) {CommissionJob j;assert(DecodeCommissionJob(invalid.checkpoint.data,j,why));j.craftFinishedRevision=0;invalid.checkpoint.data=EncodeCommissionJob(j);}
        assert(!PrepareCommissionMeeting(invalid,observation,2000,next,why));
    }
    // Optional versioned route data does not change old direct/mail payloads.
    CommissionMeetingState state;auto encoded=EncodeCommissionMeetingState(state);
    assert(DecodeCommissionMeetingState(encoded,state));
    encoded.put("attempts",101);assert(!DecodeCommissionMeetingState(encoded,state));
    encoded=EncodeCommissionMeetingState({});encoded.put("best_distance","nan");assert(!DecodeCommissionMeetingState(encoded,state));
    encoded=EncodeCommissionMeetingState({});encoded.put("no_progress_ms",120001);assert(!DecodeCommissionMeetingState(encoded,state));
    encoded=EncodeCommissionMeetingState({});encoded.put("extra",1);assert(!DecodeCommissionMeetingState(encoded,state));
    std::cout<<"living commission meeting checks passed\n";
}
