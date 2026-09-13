#include "GuildMailHandoff.h"
// Production journal/proof builders against supplied database metadata. This
// tests atomicity and refusal paths, NOT native mail gameplay or permissions.
static void GuildMailHandoffDatabase(Connection& db) {
    using namespace GuildMailFixture;
    unsigned seq=0;const auto rid=[&](){return "b399bccb-57bc-5367-84a7-"+std::to_string(100000000000ull+ ++seq);};
    auto q=Quote();auto source=Sender(q);auto uses=Uses(source,q);const auto sourceId=SqlValue(source.id);
    assert(db.Write(TaskWrite(source,0,rid(),"fixture_created")));
    source.phase=Phase::Preparing;++source.revision;
    assert(db.Write(TaskWrite(source,1,rid(),"fixture_preparing")));
    ++source.revision;
    assert(db.Write(ResourceReservationWrite(source,2,rid(),{{uses[0].before,0},{uses[1].before,0}},
        {{q.sender,q.item,q.job.entry,4,0,"bags"},{q.sender,0,0,0,q.moneyBefore,"money"}})));
    OperationRequest request;request.kind="guild_mail_send";request.effects=Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Guild);
    request.persistence=NativePersistence::Inventory;request.beforeState=EncodeGuildMailQuote(q);request.consumption=uses;
    request.transition.task=source;request.transition.expectedRevision=source.revision;
    ++request.transition.task.revision;request.transition.task.phase=Phase::Executing;
    request.transition.receipt=Proof(source).id;assert(db.Write(OperationRequestWrite(request)));
    source=request.transition.task;++source.revision;source.phase=Phase::Verifying;source.updatedAtMs=2000;
    const auto recipient=Recipient(source,q);const auto parcel=Parcel(q);const auto proof=Proof(source);
    const auto receipt=rid(),admission=rid();
    const std::string observed=R"({"mail":90014})";
    const auto handoff=GuildMailHandoffWrite(source,source.revision-1,proof,receipt,observed,q,uses,parcel,recipient,admission);
    const auto& plan=handoff.journal;
    auto unchanged=[&]() {
        assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+sourceId)=="executing");
        for(const auto& use:uses)assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(use.before.id))=="held");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task WHERE task_id="+SqlValue(recipient.id))=="0");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE claim_id="+SqlValue(handoff.recipientClaim.id))=="0");
    };
    unchanged();
    // A crash/error at EVERY statement boundary must preserve both original
    // claims and must not orphan a recipient task or duplicate a handoff.
    for(size_t stop=0;stop<plan.statements.size();++stop) {
        assert(db.Execute("START TRANSACTION"));
        for(size_t i=0;i<=stop;++i)assert(db.Execute(plan.statements[i]));
        assert(!db.Execute("INSERT INTO missing_guild_handoff_fault_table VALUES(1)"));
        assert(db.Execute("ROLLBACK"));unchanged();
    }
    // A stale receiver quote or claim revision cannot partially admit a leg.
    assert(db.Execute("UPDATE living_activity_claim SET revision=2 WHERE claim_id="+SqlValue(uses[0].before.id)));
    assert(!db.Write(plan));unchanged();
    assert(db.Execute("UPDATE living_activity_claim SET revision=1 WHERE claim_id="+SqlValue(uses[0].before.id)));
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.receiver',123) WHERE operation_id="+SqlValue(proof.id)));
    assert(!db.Write(plan));unchanged();
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.receiver',9799) WHERE operation_id="+SqlValue(proof.id)));
    // Native table shapes match the pinned CMaNGOS schema (mail is not a
    // claimed aggregate). Only this dedicated database gets supplied rows.
    assert(db.Execute("CREATE TABLE mail(id INT PRIMARY KEY,messageType INT,sender INT,receiver INT,money INT,cod INT,deliver_time BIGINT,expire_time BIGINT)"));
    assert(db.Execute("CREATE TABLE mail_items(mail_id INT,item_guid INT,item_template INT,receiver INT,PRIMARY KEY(mail_id,item_guid))"));
    assert(db.Execute("INSERT INTO mail VALUES(90014,0,9798,9799,0,0,1120,200000)"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90014,9798004,2770,9799)"));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(9798,5,470)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9798004,2770,9799,4)"));
    assert(db.Execute("INSERT INTO guild_society_supply_goal VALUES(31,'guild_mail_handoff_fixture','active','item',2770,50,0)"));
    assert(db.Execute("INSERT INTO guild_society_supply_delivery VALUES(14,31,'guild_mail_handoff_fixture',9797,9799,2770,4,90014,'mailed',0,9798004)"));
    const auto nativeProof=GuildMailNativeProof(q,source,parcel,1120,200000);
    assert(db.Scalar(nativeProof)==source.id);
    auto wrong=[&](const std::string& mutation,const std::string& repair) {
        assert(db.Execute(mutation));assert(db.Scalar(nativeProof).empty());assert(db.Execute(repair));assert(db.Scalar(nativeProof)==source.id);
    };
    wrong("UPDATE mail SET sender=9797 WHERE id=90014","UPDATE mail SET sender=9798 WHERE id=90014");
    wrong("UPDATE mail SET cod=1 WHERE id=90014","UPDATE mail SET cod=0 WHERE id=90014");
    wrong("UPDATE mail SET money=1 WHERE id=90014","UPDATE mail SET money=0 WHERE id=90014");
    wrong("UPDATE mail SET receiver=9797 WHERE id=90014","UPDATE mail SET receiver=9799 WHERE id=90014");
    wrong("UPDATE mail SET deliver_time=1121 WHERE id=90014","UPDATE mail SET deliver_time=1120 WHERE id=90014");
    wrong("UPDATE mail SET expire_time=200001 WHERE id=90014","UPDATE mail SET expire_time=200000 WHERE id=90014");
    wrong("UPDATE item_instance SET count=3 WHERE guid=9798004","UPDATE item_instance SET count=4 WHERE guid=9798004");
    wrong("UPDATE item_instance SET owner_guid=9798 WHERE guid=9798004","UPDATE item_instance SET owner_guid=9799 WHERE guid=9798004");
    wrong("UPDATE mail_items SET receiver=9798 WHERE mail_id=90014","UPDATE mail_items SET receiver=9799 WHERE mail_id=90014");
    wrong("UPDATE characters SET money=500 WHERE guid=9798","UPDATE characters SET money=470 WHERE guid=9798");
    wrong("UPDATE guild_society_supply_delivery SET donor_guid=9798 WHERE delivery_id=14","UPDATE guild_society_supply_delivery SET donor_guid=9797 WHERE delivery_id=14");
    wrong("UPDATE guild_society_supply_delivery SET deposited_quantity=4 WHERE delivery_id=14","UPDATE guild_society_supply_delivery SET deposited_quantity=0 WHERE delivery_id=14");
    wrong("UPDATE guild_society_supply_goal SET state='cancelled' WHERE goal_id='guild_mail_handoff_fixture'","UPDATE guild_society_supply_goal SET state='active' WHERE goal_id='guild_mail_handoff_fixture'");
    wrong("UPDATE guild_society_supply_execution SET enabled=0 WHERE guild_id=31","UPDATE guild_society_supply_execution SET enabled=1 WHERE guild_id=31");
    wrong("INSERT INTO mail_items VALUES(90015,9798004,2770,9799)","DELETE FROM mail_items WHERE mail_id=90015");
    wrong("INSERT INTO mail_items VALUES(90014,9798005,2770,9799)","DELETE FROM mail_items WHERE item_guid=9798005");
    wrong("INSERT INTO character_inventory(guid,item,bag,slot,item_template) VALUES(9798,9798004,0,23,2770)","DELETE FROM character_inventory WHERE item=9798004");
    assert(db.Write(plan) && db.Write(plan)); // Lost acknowledgement, one committed handoff.
    assert(db.Scalar("SELECT donor_guid FROM guild_society_supply_delivery WHERE delivery_id=14")=="9797");
    assert(db.Scalar("SELECT deposited_quantity FROM guild_society_supply_delivery WHERE delivery_id=14")=="0");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+sourceId)=="verifying");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(recipient.id))=="queued");
    for(const auto& use:uses)assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(use.before.id))=="consumed");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(handoff.recipientClaim.id))=="held");
    auto counterfeit=recipient;counterfeit.id=counterfeit.root=rid();
    const auto other=GuildMailHandoffWrite(source,source.revision-1,proof,receipt,observed,q,uses,parcel,counterfeit,rid());
    assert(!db.Write(other.journal));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task WHERE task_id="+SqlValue(counterfeit.id))=="0");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+sourceId+" AND phase='verifying'")=="1");
    UnsettledClaimBatch empty;empty.complete=true;empty.bookRevision=4;GuildDeliverySettlement settled;std::string why;
    assert(PrepareGuildMailSenderSettlement(source,source.context,empty,2001,rid(),settled,why));
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='attempted' WHERE operation_id="+SqlValue(proof.id)));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_guild_parcel_postage_and_handoff_observed' WHERE operation_id="+SqlValue(proof.id)));
    assert(!db.Write(settled.plan,true));
    assert(db.Write(settled.plan) && db.Write(settled.plan));
    assert(db.Scalar("SELECT step FROM living_activity_task WHERE task_id="+sourceId)=="guild_mail_handed_off");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(recipient.id))=="queued");
    assert(db.Scalar("SELECT phase FROM guild_society_supply_delivery WHERE delivery_id=14")=="mailed");
    assert(db.Scalar("SELECT deposited_quantity FROM guild_society_supply_delivery WHERE delivery_id=14")=="0");
}
