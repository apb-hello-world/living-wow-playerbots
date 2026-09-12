// The real common admission/execution validator must reject untyped guild work.
static void GuildDeliveryRequests() {
    GuildDeliveryJob job{41,8,497,2770,20,0,"supplies-copper",false};
    const auto encoded=EncodeGuildDeliveryJob(job);GuildDeliveryJob decoded;std::string why;
    assert(DecodeGuildDeliveryJob(encoded,decoded,why) && EncodeGuildDeliveryJob(decoded)==encoded);
    auto r=Request();r.task.source="guild_delivery";r.task.kind=Kind::GuildDelivery;r.task.priority=Priority::Delivery;
    r.task.sourceKey=GuildDeliverySourceKey(job,r.task.actor);r.task.checkpoint.data=encoded;r.task.checkpoint.step="supply_prepare";
    assert(ValidateTaskRequest(r,nullptr,r.task.context,why)==AdmissionCode::Pending);
    auto saved=r.task;r.expectedRevision=1;r.task.revision=2;r.task.phase=Phase::Preparing;
    assert(ValidateTaskRequest(r,&saved,r.task.context,why)==AdmissionCode::Pending);
    assert(SavedTaskExecutable(r.task,2,r.task.context,1000,why));
    for(unsigned field=0;field<8;++field) {
        auto j=job;
        switch(field) {
            case 0:++j.delivery;break;case 1:++j.guild;break;case 2:j.goal="other-request";break;
            case 3:++j.donor;break;case 4:++j.entry;break;case 5:++j.quantity;break;
            case 6:j.incomingMail=123;break;default:j.money=true;j.entry=0;break;
        }
        auto changed=r;changed.task.checkpoint.data=EncodeGuildDeliveryJob(j);
        assert(ValidateTaskRequest(changed,&saved,changed.task.context,why)==AdmissionCode::InvalidRequest);
    }
    for(unsigned field=0;field<7;++field) {
        auto changed=r;
        switch(field) {
            case 0:changed.task.source="other";break;case 1:changed.task.kind=Kind::Profession;break;
            case 2:changed.task.priority=Priority::Human;break;case 3:changed.task.accepted=false;break;
            case 4:changed.task.sourceKey="41";break;case 5:changed.task.parent=Receipt;break;
            default:changed.task.checkpoint.data="{\"legacy_phase\":\"carried\"}";break;
        }
        assert(!SavedTaskExecutable(changed.task,2,changed.task.context,1000,why));
    }
    auto legacy=r.task;legacy.mode=Mode::Observe;legacy.sourceKey="41";legacy.checkpoint.data="{\"legacy_phase\":\"mailed\"}";
    assert(ValidateGuildDeliveryTask(legacy,why)); // Historical import is not execution permission.
    legacy.mode=Mode::Active;assert(!ValidateGuildDeliveryTask(legacy,why));
    auto mail=job;mail.incomingMail=123;
    auto receiver=r.task;receiver.actor=receiver.context.actor=501;receiver.sourceKey=GuildDeliverySourceKey(mail,501);
    receiver.checkpoint.data=EncodeGuildDeliveryJob(mail);
    assert(ValidateGuildDeliveryTask(receiver,why));
    assert(receiver.sourceKey!=r.task.sourceKey); // No cross-actor rewrite of the original root.
    auto restored=AfterRestart(receiver,5000);assert(!SavedTaskExecutable(restored,restored.revision,receiver.context,5000,why));
    assert(restored.checkpoint.data==receiver.checkpoint.data && restored.id==receiver.id);
    auto money=job;money.money=true;money.entry=0;money.quantity=100000;
    assert(DecodeGuildDeliveryJob(EncodeGuildDeliveryJob(money),decoded,why));
    money.incomingMail=1;assert(!ValidGuildDeliveryJob(money));
    money.incomingMail=0;money.quantity=UINT32_MAX;assert(!ValidGuildDeliveryJob(money));
    for(const auto& bad:std::vector<std::string>{"{}",encoded+"x",encoded.substr(0,encoded.size()-1)+",\"entry\":2770}",
        encoded.substr(0,encoded.size()-1)+",\"unknown\":1}",std::string(1025,'x')})
        assert(!DecodeGuildDeliveryJob(bad,decoded,why));
    for(const auto& bad:std::vector<std::string>{"-1","01","1.5","4294967296","18446744073709551616"}) {
        auto text=encoded;const auto at=text.find("\"quantity\":20");assert(at!=std::string::npos);
        text.replace(at,13,"\"quantity\":"+bad);
        assert(!DecodeGuildDeliveryJob(text,decoded,why));
    }
    auto invalid=job;invalid.goal="private ' text";assert(!ValidGuildDeliveryJob(invalid));
    invalid=job;invalid.entry=0;assert(!ValidGuildDeliveryJob(invalid));
    invalid=job;invalid.quantity=0;assert(!ValidGuildDeliveryJob(invalid));
    invalid=job;invalid.delivery=0;assert(!ValidGuildDeliveryJob(invalid));
}
