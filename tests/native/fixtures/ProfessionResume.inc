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
    bad=f;bad.view.unresolvedOperation=true;fail(bad);
    bad=f;bad.view.safe=false;fail(bad);
    bad=f;bad.view.context=f.task.context;fail(bad);
    bad=f;++bad.view.revision;fail(bad);
    bad=f;bad.task.accepted=false;fail(bad);
    bad=f;bad.task.mode=Mode::Observe;fail(bad);
    for (auto phase : {Phase::Executing,Phase::Reconciling,Phase::Completed,Phase::Cancelled,Phase::Failed}) {
        bad=f;bad.task.phase=phase;fail(bad);
    }
    for (auto phase : {Phase::Queued,Phase::Preparing,Phase::Traveling,Phase::WaitingExternal,Phase::Paused,Phase::Deferred,Phase::Verifying}) {
        auto waiting=f;waiting.task.phase=phase;assert(prepare(waiting));
        if (phase==Phase::Verifying) assert(result.plan.statements[1].find("o.state IN ('verified','rejected')")!=std::string::npos);
    }
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
}
}
