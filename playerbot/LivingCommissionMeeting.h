#pragma once
#include "LivingCommissionJob.h"
#include <algorithm>

namespace LivingActivity {
struct CommissionMeetingObservation {
    uint32_t recipient=0,map=0,instance=0;
    float x=0,y=0,z=0,distance=0;
    bool arrived=false,safety=false;
    std::string blocker;
};
inline bool IsCommissionMeetingStep(const std::string& step) {return step=="commission_meeting";}

// Called only after native-operation history and current world context have
// been reconciled. No timer completes an order; arrival only enables trading.
inline bool PrepareCommissionMeeting(const Task& saved,const CommissionMeetingObservation& o,
    uint64_t now,Task& next,std::string& why) {
    why="commission_meeting_invalid";CommissionJob job;
    if(!Validate(saved,why) || !ValidateCommissionTask(saved,why) || !IsCommissionJob(saved) ||
        !DecodeCommissionJob(saved.checkpoint.data,job,why) || job.agreement.delivery!="meeting" ||
        !job.craftFinishedRevision || !saved.accepted || saved.mode!=Mode::Active || Terminal(saved.phase) ||
        saved.revision>=UINT64_MAX-1 || now>UINT64_MAX-1800000 || now<saved.updatedAtMs || saved.retryAtMs>now ||
        o.recipient!=job.agreement.recipient ||
        (!IsCommissionMeetingStep(saved.checkpoint.step) && saved.checkpoint.step!="commission_trade_prepare" &&
            saved.checkpoint.step!="commission_craft_ready"))return false;
    if(saved.phase!=Phase::Preparing && saved.phase!=Phase::Traveling && saved.phase!=Phase::Paused &&
        saved.phase!=Phase::WaitingExternal && saved.phase!=Phase::Deferred && saved.phase!=Phase::Reconciling)return false;
    next=saved;++next.revision;next.updatedAtMs=now;next.retryAtMs=0;
    next.checkpoint.step="commission_meeting";next.checkpoint.blocker.clear();
    if(!o.blocker.empty()) {
        if(!IsToken(o.blocker,64))return false;
        next.phase=o.safety?Phase::Paused:Phase::WaitingExternal;
        next.checkpoint.blocker=o.blocker;next.retryAtMs=now+(o.safety?5000:30000);
    } else if(saved.phase==Phase::Paused || saved.phase==Phase::Deferred || saved.phase==Phase::WaitingExternal) {
        next.phase=Phase::Reconciling;
    } else if(saved.phase==Phase::Reconciling) {
        next.phase=Phase::Preparing;
    } else if(o.arrived) {
        next.phase=Phase::Preparing;next.checkpoint.step="commission_trade_prepare";
        next.checkpoint.lastProgressAtMs=now;
    } else {
        CommissionMeetingState observation{o.map,o.instance,0,o.x,o.y,o.z,o.distance,0},checked;
        if(!DecodeCommissionMeetingState(EncodeCommissionMeetingState(observation),checked))return false;
        auto route=job.meeting.value_or(observation);
        const float dx=route.x-o.x,dy=route.y-o.y,dz=route.z-o.z;
        const bool changed=route.map!=o.map || route.instance!=o.instance || dx*dx+dy*dy+dz*dz>25;
        const bool progressed=!job.meeting || changed || route.bestDistance-o.distance>=5;
        const auto active=saved.phase==Phase::Traveling?std::min<uint64_t>(30000,now-saved.updatedAtMs):0;
        if(progressed) {
            const auto attempts=route.attempts;route=observation;route.attempts=attempts;
            next.checkpoint.lastProgressAtMs=now;
        } else route.noProgressMs=std::min<uint64_t>(120000,route.noProgressMs+active);
        next.checkpoint.activeElapsedMs=ActiveElapsed(saved.checkpoint.activeElapsedMs,saved.phase,active);
        next.phase=Phase::Traveling;
        if(route.noProgressMs>=120000) {
            route.attempts=std::min<uint32_t>(100,route.attempts+1);route.noProgressMs=0;
            next.phase=Phase::Deferred;next.checkpoint.blocker="commission_meeting_no_progress";
            next.retryAtMs=now+(route.attempts==1?5000:route.attempts==2?300000:1800000);
        }
        job.meeting=route;next.checkpoint.data=EncodeCommissionJob(job);
        // Bound journal traffic; ordinary movement does not write each tick.
        if(saved.phase==Phase::Traveling && next.phase==Phase::Traveling && now-saved.updatedAtMs<30000) {
            why="commission_meeting_traveling";return false;
        }
    }
    if(!CanTransition(saved,next.phase) || !PreserveCommissionIntent(saved,next,why))return false;
    why.clear();return true;
}
}
