#ifndef LIVING_NATIVE_GUILD_EVENT_H
#define LIVING_NATIVE_GUILD_EVENT_H
#include "LivingGuildEventCommitment.h"
class Player;
namespace LivingActivity {
// Exact indexed native event/RSVP read. No scheduling, joining or movement.
bool ReadNativeGuildEventAcceptance(Player&,const GuildEventCommitment&,GuildEventAcceptance&,std::string&);
bool ValidateNativeGuildEventTask(Player&,const Task&,std::string&);
}
#endif
