#ifndef LIVING_BOT_LOGIN_ADMISSION_H
#define LIVING_BOT_LOGIN_ADMISSION_H
#include <cstdint>
#include <list>
#include <set>
namespace LivingActivity {
template<class Online,class Pending,class Admit>
void LoginScheduledBots(const std::list<uint32_t>& availableBots,uint32_t onlineBotCount,
    uint32_t maximum,uint32_t maxLogins,Online online,Pending pending,Admit admit)
{
    // Reserve every already pending login before considering new candidates.
    // A higher GUID can own the final slot. Successful dispatch consumes that
    // slot immediately, not on a later update after its session appears.
    std::set<uint32_t> inFlight;
    for(const auto bot:availableBots)
        if(!online(bot) && pending(bot))inFlight.insert(bot);
    if(onlineBotCount>=maximum || inFlight.size()>=maximum-onlineBotCount)return;
    onlineBotCount+=uint32_t(inFlight.size());
    for(const auto bot:availableBots) {
        if(onlineBotCount>=maximum || !maxLogins)break;
        if(online(bot) || pending(bot))continue;
        if(admit(bot)){++onlineBotCount;--maxLogins;}
    }
}
}
#endif
