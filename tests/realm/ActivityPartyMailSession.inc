// Isolated, explicit headless-human INPUT. Native group/session permission code
// is unchanged. This proves realm execution, not network login or user QA.
// Login needs the explicit offline-human input before the simulated session
// can be attached. This test-only predicate never changes account ownership,
// the population account list, or a native group. It is absent from production.
bool LivingIsolatedPartyReconnectHuman(uint32_t member) {
    const char* mode=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const char* leader=std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT");
    if(!mode || std::string(mode)!="activity-profession-mail-party-resume-v1" || !leader)return false;
    RequireIsolatedNativeFixtureEnvironment();
    return member==std::stoul(leader);
}
namespace {
struct PartyMailSessionFixture {
    uint32_t leaderId=0;
    WorldSession* original=nullptr;
    std::unique_ptr<boost::asio::io_context> io;
    std::shared_ptr<WorldSocket> socket;
    std::unique_ptr<WorldSession> humanSession;
    Group* group=nullptr;
    float x=0,y=0,z=0,o=0;
    bool bound=false,done=false;
    bool recallInjected=false,reauthorized=false,recovered=false;
    uint32_t persistedGroup=0;
    std::string restoredReason;
    LivingActivity::PartyServiceBinding checkpointBinding;
    std::string recalledOperation;
    void Close(bool preserveGroup=false) {
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(leaderId);
        if(group && !preserveGroup) {group->Disband(true);sObjectMgr.RemoveGroup(group);delete group;}
        group=nullptr; // A preserved native group belongs to ObjectMgr, not this fixture.
        if(leader && original) {
            leader->SetSession(original);
            humanSession->SetPlayer(nullptr,0);
            if(!preserveGroup)leader->NearTeleportTo(x,y,z,o);
        }
        humanSession.reset();socket.reset();io.reset();original=nullptr;bound=false;done=true;
    }
    bool Restore(Player* bot,uint32_t expectedGroup,uint32_t expectedLeader) {
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(expectedLeader);
        if(!leader || !leader->IsInWorld() || NativeSafety(leader))return false;
        if(!bot->GetGroup() || bot->GetGroup()!=leader->GetGroup() ||
            bot->GetGroup()->GetId()!=expectedGroup || bot->GetGroup()->GetMembersCount()!=2 ||
            bot->GetGroup()->GetLeaderGuid()!=leader->GetObjectGuid())
            throw std::runtime_error("restart_requires_same_persisted_native_group:actor="+
                std::to_string(bot->GetGroup()?bot->GetGroup()->GetId():0)+":leader="+
                std::to_string(leader->GetGroup()?leader->GetGroup()->GetId():0)+":expected="+
                std::to_string(expectedGroup));
        leaderId=expectedLeader;persistedGroup=expectedGroup;group=bot->GetGroup();
        x=leader->GetPositionX();y=leader->GetPositionY();z=leader->GetPositionZ();o=leader->GetOrientation();
        original=leader->GetSession();io=std::make_unique<boost::asio::io_context>();
        socket=std::make_shared<WorldSocket>(*io);
        humanSession=std::make_unique<WorldSession>(original->GetAccountId(),socket.get(),SEC_PLAYER,
            1,0,LOCALE_enUS,"isolated_party_leader",0,0,false);
        humanSession->SetNoAnticheat();humanSession->SetPlayer(leader,leaderId);leader->SetSession(humanSession.get());
        if(!leader->isRealPlayer() || !sPlayerbotRendezvousManager.RegisterPartyAssist(bot,leader,true))
            throw std::runtime_error("restart_native_party_registration_required");
        restoredReason=sPlayerbotRendezvousManager.PartyReason(bot->GetGUIDLow());
        recovered=true;return true;
    }
    template<class State> bool Prepare(State& state,Player* bot,bool authorize=true) {
        if(!leaderId) {
            // A held fixture actor has not necessarily executed an AI action
            // yet, so it need not have an observation-cache binding. Resolve
            // only the explicitly supplied native character, not that cache.
            for(const auto candidate:{uint32_t(std::stoul(std::getenv("LIVING_WOW_NATIVE_FIXTURE_RECIPIENT")))}) {
                auto* other=sRandomPlayerbotMgr.GetPlayerBot(candidate);
                if(!other || other==bot || !other->IsInWorld() || NativeSafety(other) ||
                    other->GetGroup() || other->GetMapId()!=bot->GetMapId() ||
                    other->GetTeam()!=bot->GetTeam() || !other->GetPlayerbotAI() ||
                    state.authority.Read(candidate).lease.actor)continue;
                leaderId=candidate;x=other->GetPositionX();y=other->GetPositionY();z=other->GetPositionZ();o=other->GetOrientation();
                state.professionCohortActors.back().store(leaderId);
                other->StopMoving();other->GetMotionMaster()->Clear();
                other->NearTeleportTo(bot->GetPositionX(),bot->GetPositionY(),bot->GetPositionZ(),bot->GetOrientation());
                return false;
            }
            return false;
        }
        auto* leader=sRandomPlayerbotMgr.GetPlayerBot(leaderId);
        if(!leader || !leader->IsInWorld() || NativeSafety(leader))return false;
        if(!original) {
            original=leader->GetSession();io=std::make_unique<boost::asio::io_context>();
            socket=std::make_shared<WorldSocket>(*io);
            humanSession=std::make_unique<WorldSession>(original->GetAccountId(),socket.get(),SEC_PLAYER,
                1,0,LOCALE_enUS,"isolated_party_leader",0,0,false);
            // Match native bot-session initialization before SetPlayer invokes
            // its anticheat hook. This explicit input is isolated-only.
            humanSession->SetNoAnticheat();
            humanSession->SetPlayer(leader,leaderId);leader->SetSession(humanSession.get());
            if(!leader->isRealPlayer())throw std::runtime_error("headless_native_human_session_required");
            group=new Group();
            if(!group->Create(leader->GetObjectGuid(),leader->GetName()))throw std::runtime_error("fixture_group_create");
            sObjectMgr.AddGroup(group);
            if(!group->AddMember(bot->GetObjectGuid(),bot->GetName()))throw std::runtime_error("fixture_group_add");
            if(sPlayerbotRendezvousManager.RegisterPartyAssist(leader,leader))
                throw std::runtime_error("human_cannot_create_self_follow_session");
        }
        if(!authorize)return true; // Form the test party before delayed guild mail arrives.
        if(!bound) {
            sLivingActivityCoordinator.ReleaseTaskLease(state.authority.Read(bot->GetGUIDLow()).lease);
            if(!sPlayerbotRendezvousManager.BeginPartyFreeTime(bot,leader,"isolated_party_mail"))return false;
            bound=true;
        }
        return true;
    }
};
PartyMailSessionFixture& PartyMailFixture() {static PartyMailSessionFixture fixture;return fixture;}
}
