#ifndef LIVING_GUILD_ROUTE_PROPOSAL_H
#define LIVING_GUILD_ROUTE_PROPOSAL_H
#include <cstdint>
#include <cmath>
#include <tuple>
namespace livingguild {
// A route proposal contains immutable identifiers/coordinates, not native
// travel pointers. It cannot grant movement; current event and activity
// authority must still be validated on the world thread before installation.
struct GuildRouteProposal {
    uint32_t purpose=0,quest=0,map=0;
    int32_t entry=0;
    float x=0,y=0,z=0;
    bool Valid() const {return purpose && entry && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);}
};
struct GuildRouteEpoch {
    uint32_t actor=0,guild=0,map=0,instance=0;
    uint64_t actorGeneration=0,mapGeneration=0,groupIdentity=0,groupRevision=0;
    bool Valid() const {return actor && guild && actorGeneration && mapGeneration && groupIdentity && groupRevision;}
    bool operator==(const GuildRouteEpoch& b) const {
        return std::tie(actor,guild,map,instance,actorGeneration,mapGeneration,groupIdentity,groupRevision)==
            std::tie(b.actor,b.guild,b.map,b.instance,b.actorGeneration,b.mapGeneration,b.groupIdentity,b.groupRevision);
    }
};
inline bool FreshGuildRoute(const GuildRouteEpoch& submitted,const GuildRouteEpoch& current) {
    return submitted.Valid() && current.Valid() && submitted==current;
}
}
#endif
