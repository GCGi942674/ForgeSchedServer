#include "persistence/StateStore.h"
#include "scheduler/Scheduler.h"
#include <cassert>
#include <filesystem>
#include <sqlite3.h>
#include <unistd.h>

using namespace ForgeSched;
int main() {
    char directory[] = "/tmp/forge-recovery-XXXXXX";
    assert(mkdtemp(directory));
    const std::string path = std::string(directory) + "/state.db";
    std::string first_run, second_run;
    {
        StateStore store(path);
        WorkerManager workers;
        Scheduler scheduler(workers);
        scheduler.restoreFrom(store);
        assert(workers.registerWorker("slot", "host", 1));
        scheduler.holdWorker("slot");
        assert(scheduler.submitTask(Task(1, TaskType::REGRESSION, "case", "1")));
        assert(scheduler.schedule().empty());
        assert(scheduler.recoverWorker("slot", "idle", 0, "", 1)["action"] == "ready");
        assert(scheduler.schedule().size() == 1);
        first_run = scheduler.getTask(1)->getRunId();
        assert(scheduler.markTaskStarted(1, "slot"));
        scheduler.holdWorker("slot");
        assert(scheduler.recoverWorker("slot", "idle", 0, "", 1)["action"] == "quarantined");
        assert(scheduler.recoverWorker("slot", "active", 1, "wrong", 1)["action"] == "rejected");
        assert(scheduler.recoverWorker("slot", "active", 1, first_run, 1)["action"] == "resume");
        sqlite3* db = nullptr;
        assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TRIGGER fail_recovery BEFORE INSERT ON recovery_decisions BEGIN SELECT RAISE(ABORT,'injected'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        bool failed = false;
        try { scheduler.recoverWorker("slot", "lost", 1, first_run, 1); }
        catch (...) { failed = true; }
        assert(failed);
        assert(scheduler.getTask(1)->getStatus() == TaskStatus::RUNNING);
        assert(scheduler.getTask(1)->getRetryCount() == 0);
        assert(workers.getWorker("slot")->freeSlots() == 0);
        assert(!store.recoveryDecision(first_run, 1, "slot"));
        assert(sqlite3_exec(db, "DROP TRIGGER fail_recovery", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        assert(scheduler.recoverWorker("slot", "lost", 1, first_run, 1)["action"] == "requeued");
        assert(scheduler.recoverWorker("slot", "lost", 1, first_run, 1)["action"] == "requeued");
        assert(scheduler.getTask(1)->getRetryCount() == 1);
        assert(scheduler.schedule().empty()); // await idle handshake
        scheduler.recoverWorker("slot", "idle", 0, "", 1);
        assert(scheduler.schedule().size() == 1);
        second_run = scheduler.getTask(1)->getRunId();
        assert(second_run != first_run);
        assert(scheduler.recoverWorker("slot", "lost", 1, first_run, 1)["action"] == "requeued");
        assert(scheduler.getTask(1)->getRunId() == second_run);
        assert(scheduler.markTaskStarted(1, "slot"));
    }
    {
        StateStore store(path);
        WorkerManager workers;
        Scheduler scheduler(workers);
        scheduler.restoreFrom(store);
        assert(workers.registerWorker("slot", "host", 1));
        assert(scheduler.getTask(1)->recoveryRequired());
        assert(scheduler.recoverWorker("slot", "idle", 0, "", 1)["action"] == "quarantined");
        assert(scheduler.recoverWorker("slot", "lost", 1, first_run, 1)["action"] == "requeued");
        assert(scheduler.recoverWorker("other", "lost", 1, second_run, 1)["action"] == "rejected");
        assert(scheduler.recoverWorker("slot", "lost", 1, second_run, 1)["action"] == "failed");
        assert(scheduler.getTask(1)->getStatus() == TaskStatus::FAILED);
        assert(workers.getWorker("slot")->freeSlots() == 1);
        scheduler.recoverWorker("slot", "idle", 0, "", 1);
        Task task(2, TaskType::REGRESSION, "batch-case", "1");
        task.setPayload({{"context", {{"batch_id", "stopped"}}}});
        assert(scheduler.submitTask(task));
        assert(scheduler.schedule().size() == 1);
        auto run = scheduler.getTask(2)->getRunId();
        assert(scheduler.markTaskStarted(2, "slot"));
        scheduler.cancelBatch("stopped");
        scheduler.holdWorker("slot");
        assert(scheduler.recoverWorker("slot", "lost", 2, run, 1)["action"] == "cancelled");
        assert(scheduler.getTask(2)->getStatus() == TaskStatus::CANCELLED);
        assert(scheduler.getTask(2)->getRetryCount() == 0);
        scheduler.recoverWorker("slot", "idle", 0, "", 1);
        assert(scheduler.submitTask(Task(3, TaskType::REGRESSION, "unreceived", "1")));
        assert(scheduler.schedule().size() == 1);
        scheduler.holdWorker("slot");
        assert(scheduler.recoverWorker("slot", "idle", 0, "", 1)["action"] == "ready");
        assert(scheduler.getTask(3)->getStatus() == TaskStatus::QUEUED);
        assert(scheduler.getTask(3)->getRetryCount() == 0);
        assert(scheduler.schedule().size() == 1);
        assert(scheduler.markTaskStarted(3, "slot"));
        scheduler.holdWorker("slot"); // registration before replaying an outbox
        assert(scheduler.submitTask(Task(4, TaskType::REGRESSION, "next", "1")));
        assert(scheduler.completeTask(3, TaskStatus::SUCCEEDED));
        assert(scheduler.schedule().empty()); // result replay cannot finish the handshake
        assert(scheduler.recoverWorker("slot", "idle", 0, "", 1)["action"] == "ready");
        assert(scheduler.schedule().size() == 1);
    }
    std::filesystem::remove_all(directory);
}
