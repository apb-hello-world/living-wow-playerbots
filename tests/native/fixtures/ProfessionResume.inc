#include "ProfessionResume.h"
namespace {
void TestProfessionResumption() {
    using namespace LivingActivity;using namespace LivingActivityTest;
    ResumeFixture f;ProfessionPreparation result;std::string blocker;
    auto prepare=[&](const ResumeFixture& x) {
        return PrepareProfessionResumption(x.task,x.current,x.view,x.batch,x.balances,1200,x.receipt,result,blocker);
    };
    assert(prepare(f));
    assert(result.task.id==f.task.id && result.task.revision==4 && result.task.context==f.current);
    assert(result.task.phase==Phase::Preparing && result.task.checkpoint.data==f.task.checkpoint.data);
    assert(result.task.retryAtMs==9000 && result.task.checkpoint.activeElapsedMs==500 &&
        result.task.checkpoint.lastProgressAtMs==1050 && result.task.dueAtMs==9500);
    assert(result.plan.statements.size()==3 && result.task.ownerGeneration==f.task.ownerGeneration);
    for (const auto& sql : result.plan.statements) {
        assert(sql.find("UPDATE living_activity_claim")==std::string::npos);
        assert(sql.find("INSERT INTO living_activity_operation")==std::string::npos);
    }
    auto fail=[&](const ResumeFixture& x) {assert(!prepare(x) && !blocker.empty() && result.plan.statements.empty());};
    auto bad=f;bad.view.complete=false;fail(bad);
    auto protectedStock=f;protectedStock.view.stock.clear();
    protectedStock.view.readinessBlocker="profession_material_has_legacy_commitment";
    assert(prepare(protectedStock));
    assert(result.task.context==f.current && result.task.checkpoint.data==f.task.checkpoint.data &&
        result.task.retryAtMs==f.task.retryAtMs && result.task.checkpoint.activeElapsedMs==500);
    auto rebound=result.task;auto inspected=protectedStock.view;
    inspected.context=rebound.context;inspected.revision=rebound.revision;
    assert(NextProfessionStep(rebound,inspected).step==ProfessionStep::Defer);
    assert(NextProfessionStep(rebound,inspected).quantities.empty());
    bad=protectedStock;bad.view.readinessBlocker="native_effect_unknown";fail(bad);
    bad=protectedStock;bad.view.unresolvedOperation=true;fail(bad);
    bad=protectedStock;bad.view.safe=false;fail(bad);
    bad=f;bad.view.unresolvedOperation=true;fail(bad);
    bad=f;bad.view.safe=false;fail(bad);
    bad=f;bad.view.context=f.task.context;fail(bad);
    bad=f;++bad.view.revision;fail(bad);
    bad=f;bad.task.accepted=false;fail(bad);
    bad=f;bad.task.mode=Mode::Observe;fail(bad);
    for (auto phase : {Phase::Executing,Phase::Completed,Phase::Cancelled,Phase::Failed}) {
        bad=f;bad.task.phase=phase;fail(bad);
    }
    for (auto phase : {Phase::Queued,Phase::Preparing,Phase::Traveling,Phase::WaitingExternal,Phase::Paused,Phase::Deferred,Phase::Verifying,Phase::Reconciling}) {
        auto waiting=f;waiting.task.phase=phase;assert(prepare(waiting));
        if (phase==Phase::Verifying) assert(result.plan.statements[1].find("o.state IN ('verified','rejected')")!=std::string::npos);
    }
    auto wake=f;wake.task.phase=Phase::Reconciling;
    wake.task.checkpoint.blocker="purchase_daily_limit";
    assert(prepare(wake)); // Restart between retry wake-up and preparation.
    assert(result.task.phase==Phase::Preparing && result.task.context==f.current);
    assert(result.task.retryAtMs==f.task.retryAtMs && result.task.checkpoint.activeElapsedMs==500 &&
        result.task.checkpoint.lastProgressAtMs==1050 && result.task.checkpoint.data==f.task.checkpoint.data);
    assert(result.plan.statements[1].find("phase="+SqlValue("reconciling"))!=std::string::npos);
    assert(result.plan.statements[1].find("o.state IN ('intent','reconciling')")!=std::string::npos);
    assert(result.plan.statements[1].find("c.state NOT IN ('consumed','released')")!=std::string::npos);
    bad=wake;bad.view.unresolvedOperation=true;fail(bad);
    bad=wake;bad.view.complete=false;fail(bad);
    bad=wake;bad.batch.claims[0].state="reconciling";fail(bad);
    bad=wake;bad.balances[0].quantity=0;fail(bad);
    bad=wake;bad.view.safe=false;fail(bad);
    bad=wake;bad.view.readinessBlocker="native_effect_unknown";fail(bad);
    bad=f;bad.current.boot.clear();bad.view.context=bad.current;fail(bad);
    bad=f;bad.current.actorGeneration=0;bad.view.context=bad.current;fail(bad);
    bad=f;bad.current.mapGeneration=0;bad.view.context=bad.current;fail(bad);
    bad=f;bad.current.session="group:1";bad.view.context=bad.current;fail(bad);
    bad=f;bad.task.context=f.current;fail(bad);
    auto zone=f;zone.task.context=f.current;++zone.current.mapGeneration;zone.view.context=zone.current;
    assert(prepare(zone)); // Same-boot zoning invalidates old grants, not the obligation.
    bad=f;bad.batch.complete=false;fail(bad);
    bad=f;bad.batch.bookRevision=0;fail(bad);
    bad=f;bad.batch.claims.push_back(bad.batch.claims.front());fail(bad);
    bad=f;bad.balances.clear();fail(bad);
    bad=f;bad.balances[0].quantity=0;fail(bad);
    bad=f;++bad.balances[0].nativeReference;fail(bad);
    bad=f;bad.batch.claims[0].state="in_transfer";fail(bad);
    bad=f;bad.batch.claims[0].state="reconciling";fail(bad);
    bad=f;bad.batch.claims[0].task=f.receipt;fail(bad);
    auto proposed=f;auto& p=proposed.batch.claims[0];p.state="proposed";p.itemGuid=0;p.nativeReference=0;
    proposed.balances.clear();assert(prepare(proposed));
    auto empty=f;empty.batch.claims.clear();empty.balances.clear();assert(prepare(empty));
    auto money=f;auto& reserve=money.batch.claims[0];
    reserve.itemGuid=reserve.itemEntry=reserve.quantity=0;reserve.nativeReference=0;
    reserve.copper=20;reserve.location="money";money.balances={{704,0,0,0,20,"money"}};
    assert(prepare(money));money.balances[0].copper=19;fail(money);
    auto bank=f;bank.batch.claims[0].location="bank";bank.batch.claims[0].nativeReference=0;
    bank.balances[0].location="bank";bank.balances[0].nativeReference=0;assert(prepare(bank));
    auto ready=f;ready.view.retryReady=true;ready.view.stock={{765,1},{2449,1},{3371,5}};
    assert(prepare(ready)); // Still only rebinds; native execution is a separate admission.
    assert(f.batch.claims[0].state=="held" && f.batch.claims[0].revision==1);
    {
        auto saved=f.task;saved.phase=Phase::Executing;saved.checkpoint.step="profession_capacity_sale";
        ResourceClaim c=f.batch.claims[0];c.itemEntry=7073;c.quantity=5;c.location="bags";c.nativeReference=0;
        UnsettledClaimBatch batch;batch.complete=true;batch.bookRevision=8;batch.claims={c};
        std::vector<NativeResourceBalance> balances={{704,c.itemGuid,7073,5,0,"bags"}};
        const NativeItemStack item{704,c.itemGuid,7073,5,0,24};
        StoredCraftOperation row;row.acknowledged=true;row.journalDigest=std::string(64,'a');
        row.receipt={"aa2efbdf-f0ec-4539-b840-299847977003",saved.id,"capacity_vendor_sale","","",saved.revision,OperationState::Intent};
        const std::string quote="{\"actor\":704,\"guid\":110,\"entry\":7073,\"quantity\":5,\"unit_copper\":6,\"money\":100,\"copper\":30,\"count_before\":5,\"vendor\":12,\"vendor_entry\":13,\"from\":65304,\"capacity_entry\":2454,\"capacity_quantity\":1}";
        row.beforeState="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(quote,{{c,5}})+"}";row.afterState="{}";
        ProfessionHistory history;history.task=saved.id;history.revision=saved.revision;history.complete=true;
        history.unresolvedOperation=true;history.interruptedCapacity=row;
        InterruptedCapacityIntent decoded;
        assert(DecodeInterruptedCapacityIntent(saved,row,decoded,blocker));
        assert(decoded.position==65304 && SameResourceClaim(decoded.claim,c));
        auto recover=[&](const Task& task,const ProfessionHistory& h,const UnsettledClaimBatch& b,
            const NativeItemStack& physical,uint32_t wallet=100) {
            return PrepareInterruptedCapacity(task,f.current,h,b,balances,{physical,65304,wallet,5},1200,f.receipt,result,blocker);
        };
        assert(recover(saved,history,batch,item));
        assert(result.task.phase==Phase::Verifying && result.task.checkpoint.step=="profession_prepare" && result.task.id==saved.id);
        assert(result.plan.statements.size()==4 && result.plan.statements[1].find("SHA2")!=std::string::npos);
        for(const auto& sql:result.plan.statements)assert(sql.find("UPDATE living_activity_claim")==std::string::npos &&
            sql.find("DELETE FROM item_instance")==std::string::npos && sql.find("SET money")==std::string::npos);
        auto wrong=saved;wrong.context=f.current;assert(!recover(wrong,history,batch,item));
        auto changed=item;++changed.count;assert(!recover(saved,history,batch,changed));
        assert(!recover(saved,history,batch,item,130));
        auto changedClaims=batch;++changedClaims.claims[0].revision;assert(!recover(saved,history,changedClaims,item));
        for(unsigned fault=0;fault<7;++fault) {
            auto bad=row;
            if(fault==0)bad.afterState="{\"money\":130}";
            if(fault==1)bad.receipt.state=OperationState::Verified;
            if(fault==2)bad.receipt.evidence="native_save_capture_requires_reconciliation";
            if(fault==3)bad.receipt.taskRevision--;
            if(fault==4)bad.acknowledged=false;
            if(fault==5)bad.journalDigest.clear();
            if(fault==6)bad.beforeState="{}";
            assert(!DecodeInterruptedCapacityIntent(saved,bad,decoded,blocker));
        }
        ProfessionHistoryRow dbrow={"704",std::to_string(saved.revision),"1",row.receipt.id,saved.id,
            std::to_string(saved.revision),"capacity_vendor_sale","intent","",row.beforeState,"{}","",row.journalDigest};
        ProfessionHistoryCursor cursor;assert(cursor.Begin(saved,{dbrow},blocker));assert(cursor.Advance(blocker));
        assert(cursor.Result().complete && cursor.Result().interruptedCapacity && !cursor.Result().interruptedCraft);
        // Bank preparation shares the same no-effect recovery, but also
        // requires the quoted destination empty and the total stock unchanged.
        auto bank=saved;bank.checkpoint.step="profession_capacity_bank";
        auto bankRow=row;bankRow.receipt.kind="bank_deposit";
        const auto transfer="{\"claim\":\""+c.id+"\",\"revision\":1,\"guid\":110,\"entry\":7073,\"quantity\":5,\"destination\":\"bank\"}";
        bankRow.beforeState="{\"effects\":4,\"persistence\":1,\"native\":{\"native\":{\"actor\":704,\"guid\":110,\"entry\":7073,\"quantity\":5,\"banker\":12,\"banker_entry\":13,\"from\":65304,\"to\":65319,\"bag_before\":5,\"total_before\":7,\"deposit\":true,\"money_before\":100},\"transfer\":"+transfer+"}}";
        assert(DecodeInterruptedCapacityIntent(bank,bankRow,decoded,blocker) && decoded.bankDeposit && decoded.totalCount==7);
        auto bankHistory=history;bankHistory.interruptedCapacity=bankRow;
        CapacityRestoreSnapshot physical{item,65304,100,5,7,0,true};
        auto recoverBank=[&](const CapacityRestoreSnapshot& p) {
            return PrepareInterruptedCapacity(bank,f.current,bankHistory,batch,balances,p,1200,f.receipt,result,blocker);
        };
        assert(recoverBank(physical));
        assert(result.plan.statements[1].find("COALESCE(SUM(i.count),0)")!=std::string::npos);
        assert(result.plan.statements[1].find("AND bag=0 AND slot=39")!=std::string::npos);
        auto wrongBank=physical;wrongBank.destinationEmpty=false;assert(!recoverBank(wrongBank));
        wrongBank=physical;++wrongBank.totalCount;assert(!recoverBank(wrongBank));
        wrongBank=physical;++wrongBank.item.count;assert(!recoverBank(wrongBank));
        wrongBank=physical;++wrongBank.money;assert(!recoverBank(wrongBank));
        assert(!DecodeInterruptedCapacityIntent(saved,bankRow,decoded,blocker));
        auto malformed=bankRow;malformed.beforeState.insert(malformed.beforeState.find("\"deposit\""),"\"merge_guid\":11,");
        assert(!DecodeInterruptedCapacityIntent(bank,malformed,decoded,blocker));
        dbrow[6]="bank_deposit";dbrow[9]=bankRow.beforeState;
        assert(cursor.Begin(bank,{dbrow},blocker) && cursor.Advance(blocker));
        assert(cursor.Result().complete && cursor.Result().interruptedCapacity && !cursor.Result().interruptedCraft);
    }
}
}
