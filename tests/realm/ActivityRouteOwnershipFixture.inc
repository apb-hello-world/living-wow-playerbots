// Actual native route/container types. No game actor, movement or transaction.
#include "playerbot/TravelNode.h"
namespace {
void VerifyNativeRouteOwnership() {
    using namespace ai;
    auto require=[](bool value){if(!value)throw std::runtime_error("route_temporary_lifetime_mismatch");};
    struct CountedPortal final : PortalNode {
        CountedPortal(TravelNode* base,unsigned& destroyed):PortalNode(base),destroyed(destroyed) {}
        ~CountedPortal(){++destroyed;}
        unsigned& destroyed;
    };
    TravelNode borrowed;unsigned destroyed=0;
    for(unsigned iteration=0;iteration<2000;++iteration) {
        std::weak_ptr<TravelNode> weak;
        {
            auto node=std::make_shared<CountedPortal>(&borrowed,destroyed);weak=node;
            node->SetPortal(&borrowed,&borrowed,8690);
            TravelNodeRoute route({&borrowed,node.get()},{node});node.reset();
            auto copy=route;
            route.cleanTempNodes();route.cleanTempNodes();
            require(route.isEmpty() && !weak.expired() && copy.getNodes().size()==2);
            TravelNodeRoute moved=std::move(copy);
            require(!weak.expired() && moved.getNodes().back()==weak.lock().get());
        }
        require(weak.expired() && destroyed==iteration+1);
    }
    // Exception/abandoned-route path: no explicit clean call is necessary.
    std::weak_ptr<TravelNode> abandoned;
    try {
        auto node=std::make_shared<CountedPortal>(&borrowed,destroyed);abandoned=node;
        TravelNodeRoute route({node.get()},{node});node.reset();
        throw std::runtime_error("test_early_exit");
    } catch(const std::runtime_error& error) {require(std::string(error.what())=="test_early_exit");}
    require(abandoned.expired() && destroyed==2001);
    // An unsuccessful fallback keeps its start alive across candidates, then
    // releases it; a successful route retains it beyond the planning scope.
    std::weak_ptr<TravelNode> fallback;TravelNodeRoute selected;
    {
        auto node=std::make_shared<CountedPortal>(&borrowed,destroyed);fallback=node;
        for(unsigned candidate=0;candidate<5;++candidate) {TravelNodeRoute rejected;require(rejected.isEmpty());}
        selected.setNodes({&borrowed,node.get()});selected.addTempNode(node);
    }
    require(!fallback.expired());selected=TravelNodeRoute();
    require(fallback.expired() && destroyed==2002);
    // Borrowed graph storage was never adopted/deleted.
    require(borrowed.getPaths()->empty());
}
}
