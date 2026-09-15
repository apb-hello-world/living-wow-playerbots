// Methods of the existing coordinator State. One bounded asynchronous dispatch
// shares its native database queue; no bot timer, extra owner or mutation worker.
bool DeliverCommissionProjection(uint64_t now) {
    if(projectionSend.valid()) {
        if(projectionSend.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
        bool acknowledged=false;
        try{acknowledged=projectionSend.get();}catch(const std::exception&){}
        if(acknowledged) {
            // Lost database acknowledgement is safe: the next indexed read
            // replays the same immutable event; the gateway deduplicates it.
            std::string ids;
            for(const auto& id:projectionIds){if(!ids.empty())ids+=',';ids+=SqlValue(id);}
            CharacterDatabase.Execute(("UPDATE living_activity_transition SET delivered_at_ms="+std::to_string(now)+
                " WHERE delivered_at_ms=0 AND transition_id IN ("+ids+")").c_str());
            projectionReceipts+=projectionIds.size();projectionFailures=0;projectionBlocker.clear();
        }else{++projectionFailures;projectionBlocker="activity_projection_gateway_unavailable";}
        projectionIds.clear();
        projectionDue=now+(acknowledged?10000:std::min<uint64_t>(300000,10000ULL<<std::min<uint64_t>(projectionFailures,5)));
    }
    if(now<projectionDue || projectionSend.valid())return false;
    projectionDue=now+10000;ioPending=true;
    std::string fixtureScope;
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    // Historical copied fixtures deliberately reused a transaction label.
    // Select the explicit modern broker case without editing those receipts.
    // This filter does not exist in a production binary.
    const char* requested=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
    const char* fixture=std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    if(requested && *requested && fixture && std::string(fixture)=="activity-commission-replay-v1") {
        if(!IsUuid(requested)){ioPending=false;projectionBlocker="invalid_isolated_projection_task";return false;}
        fixtureScope=" AND q.task_id="+SqlValue(requested);
    }
#endif
    // Current task projection can be newer than this transition. Event identity
    // stays immutable while the status consumer rejects stale task revisions.
    const auto query="SELECT * FROM (SELECT q.transition_id,q.task_id,q.task_revision,q.actor_guid,q.phase,q.code,q.occurred_at_ms,"
        "(SELECT "+PersistedTaskProjection()+" FROM living_activity_task WHERE task_id=q.task_id) payload "
        "FROM living_activity_transition q WHERE q.delivered_at_ms=0"+fixtureScope+" AND EXISTS(SELECT 1 FROM living_activity_task t "
        "WHERE t.task_id=q.task_id AND t.source='commission_job' AND t.mode='active' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(t.checkpoint,'$.agreement.delivery'))='mail') ORDER BY q.sequence_id LIMIT 8) limited "
        "UNION ALL SELECT '','',0,0,'','',0,'{}'";
    if(!CharacterDatabase.AsyncQuery([this](QueryResult* result) {
        ioPending=false;++projectionReads;
        if(!result){projectionBlocker="activity_projection_read_unavailable";projectionDue=NowMs()+30000;return;}
        std::set<std::string> ids;std::ostringstream payload;payload<<"{\"version\":1,\"events\":[";
        bool first=true;
        auto quote=[](const std::string& s){return '"'+PlayerbotLLMInterface::SanitizeForJson(s)+'"';};
        try {
            if(result->GetFieldCount()!=8)throw std::runtime_error("activity_projection_invalid_columns");
            do {
                auto* f=result->Fetch();Task task;std::string why;
                if(f[0].GetCppString().empty())continue;
                if(!DecodeTaskProjection(f[7].GetCppString(),task,why) || !ValidateCommissionTask(task,why) ||
                    task.id!=f[1].GetCppString() || task.actor!=f[3].GetUInt32() || task.revision<f[2].GetUInt64())
                    throw std::runtime_error("activity_projection_invalid_task");
                const auto id=f[0].GetCppString(),status=sPlayerbotActionBroker.ManagedCommissionStatus(task);
                if(!IsUuid(id) || !ids.insert(id).second || ids.size()>8 || status.empty())
                    throw std::runtime_error("activity_projection_invalid_event");
                if(!first)payload<<',';first=false;
                payload<<"{\"event_id\":"<<quote(id)<<",\"task_id\":"<<quote(task.id)
                    <<",\"revision\":"<<f[2].GetUInt64()<<",\"actor_guid\":"<<task.actor
                    <<",\"phase\":"<<quote(f[4].GetCppString())<<",\"code\":"<<quote(f[5].GetCppString())
                    <<",\"occurred_at_ms\":"<<f[6].GetUInt64()<<",\"action\":"<<status<<'}';
            }while(result->NextRow());
            if(ids.empty()){projectionBlocker.clear();return;}
            payload<<"]}";const auto body=payload.str();
            if(body.size()>65536)throw std::runtime_error("activity_projection_payload_limit");
            projectionIds=ids;
            projectionSend=std::async(std::launch::async,[body,ids]() {
                std::vector<std::string> debug;
                return ProjectionAcknowledged(PlayerbotLLMInterface::Generate(body,3,2,debug,true,"/v2/activity-events"),ids);
            });
            projectionBlocker="activity_projection_sending";
        }catch(const std::exception& e){projectionIds.clear();projectionBlocker=e.what();projectionDue=NowMs()+30000;}
    },query.c_str())){ioPending=false;projectionBlocker="activity_projection_queue_unavailable";projectionDue=now+30000;}
    return true;
}
