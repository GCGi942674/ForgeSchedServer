#include "persistence/StateStore.h"
#include "task/TaskStatus.h"
#include "task/TaskType.h"
#include <sqlite3.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <limits>

namespace GCRP {
namespace {
void check(int code, sqlite3* db) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(std::string("SQLite: ") + sqlite3_errmsg(db));
}
struct Statement {
    sqlite3_stmt* value{nullptr};
    sqlite3* db;
    Statement(sqlite3* d, const char* sql) : db(d) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const std::string& data) {
        check(sqlite3_bind_text(value, index, data.c_str(), static_cast<int>(data.size()), SQLITE_TRANSIENT), db);
    }
    void number(int index, int64_t data) { check(sqlite3_bind_int64(value, index, data), db); }
    void nullable(int index, const std::string& data) {
        if (data.empty()) check(sqlite3_bind_null(value, index), db);
        else text(index, data);
    }
    void run() { check(sqlite3_step(value), db); }
    void reset() {
        check(sqlite3_reset(value), db);
        check(sqlite3_clear_bindings(value), db);
    }
};
std::string column(sqlite3_stmt* statement, int index) {
    const auto* data = sqlite3_column_text(statement, index);
    return data ? reinterpret_cast<const char*>(data) : std::string();
}
int64_t millis(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}
std::chrono::system_clock::time_point fromMillis(int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}
TaskStatus statusFrom(const std::string& value) {
    for (auto status : {TaskStatus::PENDING, TaskStatus::QUEUED, TaskStatus::ASSIGNED,
                        TaskStatus::RUNNING, TaskStatus::SUCCEEDED, TaskStatus::FAILED,
                        TaskStatus::TIMEOUT, TaskStatus::CANCELLED})
        if (toString(status) == value) return status;
    throw std::runtime_error("invalid persisted task status");
}
std::string batchId(const Task& task) {
    const auto& payload = task.getPayload();
    if (payload.is_object() && payload.contains("context") &&
        payload["context"].is_object() &&
        payload["context"].contains("batch_id") &&
        payload["context"]["batch_id"].is_string())
        return payload["context"]["batch_id"].get<std::string>();
    return {};
}
int64_t checkedId(TaskId id) {
    if (id == 0 || id > static_cast<TaskId>(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("task id outside SQLite range");
    return static_cast<int64_t>(id);
}
} // namespace

void StateStore::exec(const char* sql) const {
    char* message = nullptr;
    int result = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string error = message ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
        throw std::runtime_error("SQLite: " + error);
    }
}

StateStore::StateStore(const std::string& path, int busy_timeout_ms) {
    if (path.empty() || path == ":memory:") throw std::runtime_error("persistent database path required");
    if (busy_timeout_ms < 1 || busy_timeout_ms > 60000)
        throw std::runtime_error("invalid SQLite busy timeout");
    const std::filesystem::path db_path(path);
    if (!db_path.parent_path().empty()) std::filesystem::create_directories(db_path.parent_path());
    lock_fd_ = ::open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("database already used by another server");
    try {
        check(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), db_);
        check(sqlite3_busy_timeout(db_, busy_timeout_ms), db_);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA foreign_keys=ON");
        int schema_version = 0;
        {
            Statement version(db_, "PRAGMA user_version");
            check(sqlite3_step(version.value), db_);
            schema_version = sqlite3_column_int(version.value, 0);
        }
        if (schema_version < 0 || schema_version > 3)
            throw std::runtime_error("unsupported GCRP database schema version");
        exec("BEGIN IMMEDIATE");
        try {
            exec("CREATE TABLE IF NOT EXISTS batches (batch_id TEXT PRIMARY KEY, stop_requested INTEGER NOT NULL DEFAULT 0 CHECK(stop_requested IN (0,1)))");
            exec("CREATE TABLE IF NOT EXISTS tasks (task_id INTEGER PRIMARY KEY AUTOINCREMENT, batch_id TEXT REFERENCES batches(batch_id), task_type TEXT NOT NULL CHECK(task_type='REGRESSION'), target TEXT NOT NULL, revision TEXT NOT NULL, priority INTEGER NOT NULL CHECK(priority BETWEEN -100 AND 100), status TEXT NOT NULL CHECK(status IN ('PENDING','QUEUED','ASSIGNED','RUNNING','SUCCEEDED','FAILED','TIMEOUT','CANCELLED')), payload_json TEXT NOT NULL, execution_json TEXT, worker_id TEXT, retry_count INTEGER NOT NULL DEFAULT 0 CHECK(retry_count>=0), queue_sequence INTEGER NOT NULL CHECK(queue_sequence>=0), run_id TEXT, recovery_required INTEGER NOT NULL DEFAULT 0 CHECK(recovery_required IN (0,1)), created_at_ms INTEGER NOT NULL, started_at_ms INTEGER, finished_at_ms INTEGER)");
            exec("CREATE TABLE IF NOT EXISTS task_runs (run_id TEXT PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(task_id), attempt_no INTEGER NOT NULL CHECK(attempt_no>0), worker_id TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('ASSIGNED','RUNNING','SUCCEEDED','FAILED','TIMEOUT','DISPATCH_FAILED')), assigned_at_ms INTEGER NOT NULL, started_at_ms INTEGER, finished_at_ms INTEGER, execution_json TEXT, UNIQUE(task_id,attempt_no))");
            exec("CREATE TABLE IF NOT EXISTS submission_keys (submission_key TEXT PRIMARY KEY, request_json TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(task_id))");
            exec("CREATE TABLE IF NOT EXISTS recovery_decisions (run_id TEXT PRIMARY KEY REFERENCES task_runs(run_id), task_id INTEGER NOT NULL REFERENCES tasks(task_id), worker_id TEXT NOT NULL, decision TEXT NOT NULL CHECK(decision IN ('requeued','failed','cancelled')))");
            exec("CREATE INDEX IF NOT EXISTS tasks_queue ON tasks(status,recovery_required,priority,queue_sequence)");
            exec("CREATE INDEX IF NOT EXISTS tasks_batch ON tasks(batch_id,status)");
            exec("CREATE TABLE IF NOT EXISTS worker_management (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
            exec("PRAGMA user_version=3");
            exec("UPDATE tasks SET recovery_required=1 WHERE status IN ('ASSIGNED','RUNNING')");
            exec("COMMIT");
        } catch (...) { exec("ROLLBACK"); throw; }
        {
            Statement check_db(db_, "PRAGMA quick_check");
            check(sqlite3_step(check_db.value), db_);
            if (column(check_db.value, 0) != "ok")
                throw std::runtime_error("SQLite integrity check failed");
        }
    } catch (...) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
        throw;
    }
}

StateStore::~StateStore() {
    if (db_) sqlite3_close(db_);
    if (lock_fd_ >= 0) ::close(lock_fd_);
}

std::pair<TaskId,uint64_t> StateStore::highWaterMarks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT COALESCE(MAX(task_id),0),COALESCE(MAX(queue_sequence),0) FROM tasks");
    check(sqlite3_step(query.value),db_);
    return {static_cast<TaskId>(sqlite3_column_int64(query.value,0)),static_cast<uint64_t>(sqlite3_column_int64(query.value,1))};
}

nlohmann::json StateStore::statusCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);nlohmann::json result=nlohmann::json::object();
    Statement query(db_,"SELECT status,COUNT(*) FROM tasks GROUP BY status");
    for(;;){auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)break;check(step,db_);result[column(query.value,0)]=sqlite3_column_int64(query.value,1);}
    return result;
}

std::vector<PersistedTask> StateStore::loadTasks(TaskId id, bool active_only,const std::string& batch_id) const {
    std::vector<PersistedTask> result;
    visitTasks([&](Task&& task,uint64_t sequence){result.push_back({std::move(task),sequence});},id,active_only,batch_id);
    return result;
}

void StateStore::visitTasks(const std::function<void(Task&&,uint64_t)>& visitor,TaskId id,bool active_only,const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql="SELECT task_id,task_type,target,revision,priority,status,payload_json,execution_json,COALESCE(worker_id,''),retry_count,queue_sequence,COALESCE(run_id,''),recovery_required,created_at_ms,COALESCE(started_at_ms,0),COALESCE(finished_at_ms,0) FROM tasks WHERE 1=1";
    if(id)sql+=" AND task_id=?";
    if(active_only)sql+=" AND status IN ('PENDING','QUEUED','ASSIGNED','RUNNING')";
    if(!batch_id.empty())sql+=" AND batch_id=?";
    sql+=" ORDER BY task_id";Statement rows(db_,sql.c_str());int parameter=1;
    if(id)rows.number(parameter++,checkedId(id));
    if(!batch_id.empty())rows.text(parameter,batch_id);
    while (true) {
        int step = sqlite3_step(rows.value);
        if (step == SQLITE_DONE) break;
        check(step, db_);
        auto type = column(rows.value, 1) == "REGRESSION" ? TaskType::REGRESSION : TaskType::UNKNOWN;
        if (type == TaskType::UNKNOWN) throw std::runtime_error("invalid persisted task type");
        Task task(static_cast<TaskId>(sqlite3_column_int64(rows.value, 0)), type,
                  column(rows.value, 2), column(rows.value, 3));
        auto payload = nlohmann::json::parse(column(rows.value, 6));
        auto execution = sqlite3_column_type(rows.value, 7) == SQLITE_NULL ?
            nlohmann::json(nullptr) : nlohmann::json::parse(column(rows.value, 7));
        auto sequence = static_cast<uint64_t>(sqlite3_column_int64(rows.value, 10));
        task.setPriority(sqlite3_column_int(rows.value, 4));
        task.restore(statusFrom(column(rows.value, 5)), column(rows.value, 8),
                     sqlite3_column_int(rows.value, 9), std::move(payload), std::move(execution),
                     sequence, column(rows.value, 11), sqlite3_column_int(rows.value, 12) != 0,
                     fromMillis(sqlite3_column_int64(rows.value, 13)),
                     fromMillis(sqlite3_column_int64(rows.value, 14)),
                     fromMillis(sqlite3_column_int64(rows.value, 15)));
        visitor(std::move(task), sequence);
    }
}

std::vector<std::string> StateStore::loadStoppedBatches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement rows(db_, "SELECT batch_id FROM batches WHERE stop_requested=1");
    std::vector<std::string> result;
    while (true) {
        int step = sqlite3_step(rows.value);
        if (step == SQLITE_DONE) break;
        check(step, db_);
        result.push_back(column(rows.value, 0));
    }
    return result;
}

std::optional<std::pair<TaskId, std::string>> StateStore::findSubmission(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT task_id,request_json FROM submission_keys WHERE submission_key=?");
    query.text(1, key);
    int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return std::make_pair(static_cast<TaskId>(sqlite3_column_int64(query.value, 0)),
                          column(query.value, 1));
}

std::optional<std::string> StateStore::recoveryDecision(const std::string& run_id,
        TaskId task_id, const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT decision FROM recovery_decisions WHERE run_id=? AND task_id=? AND worker_id=?");
    query.text(1, run_id);
    query.number(2, checkedId(task_id));
    query.text(3, worker_id);
    const int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return column(query.value, 0);
}

std::optional<std::string> StateStore::archiveOwner(TaskId task_id, const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT worker_id FROM task_runs WHERE task_id=? AND run_id=? "
                         "AND status IN ('SUCCEEDED','FAILED','TIMEOUT')");
    query.number(1, checkedId(task_id));
    query.text(2, run_id);
    const int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return column(query.value, 0);
}

void StateStore::save(const std::vector<Task>& tasks,
                      const std::optional<std::string>& stopped_batch,
                      const std::optional<std::string>& run_status_override,
                      const std::optional<std::pair<std::string, std::string>>& submission,
                      const std::optional<std::pair<std::string, std::string>>& recovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
        Statement batch(db_, "INSERT OR IGNORE INTO batches(batch_id) VALUES(?)");
        Statement stop(db_, "UPDATE batches SET stop_requested=1 WHERE batch_id=?");
        Statement insert(db_, "INSERT OR IGNORE INTO tasks(task_id,batch_id,task_type,target,revision,priority,status,payload_json,execution_json,worker_id,retry_count,queue_sequence,run_id,recovery_required,created_at_ms,started_at_ms,finished_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Statement update(db_, "UPDATE tasks SET batch_id=?,task_type=?,target=?,revision=?,priority=?,status=?,payload_json=?,execution_json=?,worker_id=?,retry_count=?,queue_sequence=?,run_id=?,recovery_required=?,created_at_ms=?,started_at_ms=?,finished_at_ms=? WHERE task_id=?");
        Statement run_insert(db_, "INSERT OR IGNORE INTO task_runs(run_id,task_id,attempt_no,worker_id,status,assigned_at_ms) VALUES(?,?,(SELECT COALESCE(MAX(attempt_no),0)+1 FROM task_runs WHERE task_id=?),?,?,?)");
        Statement run_update(db_, "UPDATE task_runs SET status=?,started_at_ms=?,finished_at_ms=?,execution_json=? WHERE run_id=?");
        Statement submit(db_, "INSERT INTO submission_keys(submission_key,request_json,task_id) VALUES(?,?,?)");
        if (stopped_batch) {
            batch.text(1, *stopped_batch); batch.run(); batch.reset();
            stop.text(1, *stopped_batch); stop.run();
        }
        for (const auto& task : tasks) {
            const auto batch_id = batchId(task);
            if (!batch_id.empty()) {
                batch.text(1, batch_id); batch.run(); batch.reset();
            }
            auto bind = [&](Statement& statement, int offset, bool with_id) {
                int i = offset;
                if (with_id) statement.number(i++, checkedId(task.getId()));
                statement.nullable(i++, batch_id);
                statement.text(i++, toString(task.getType()));
                statement.text(i++, task.getTarget());
                statement.text(i++, task.getRevision());
                statement.number(i++, task.getPriority());
                statement.text(i++, toString(task.getStatus()));
                statement.text(i++, task.getPayload().dump());
                statement.nullable(i++, task.getExecutionSummary().is_null() ? "" : task.getExecutionSummary().dump());
                statement.nullable(i++, task.getWorkerId());
                statement.number(i++, task.getRetryCount());
                statement.number(i++, static_cast<int64_t>(task.getQueueSequence()));
                statement.nullable(i++, task.getRunId());
                statement.number(i++, task.recoveryRequired() ? 1 : 0);
                statement.number(i++, millis(task.getCreatedAt()));
                statement.number(i++, millis(task.getStartedAt()));
                statement.number(i++, millis(task.getFinishedAt()));
            };
            bind(insert, 1, true); insert.run(); insert.reset();
            bind(update, 1, false); update.number(17, checkedId(task.getId())); update.run(); update.reset();
            if (!task.getRunId().empty() &&
                (run_status_override || !task.getWorkerId().empty())) {
                run_insert.text(1, task.getRunId());
                run_insert.number(2, checkedId(task.getId()));
                run_insert.number(3, checkedId(task.getId()));
                run_insert.text(4, task.getWorkerId());
                run_insert.text(5, "ASSIGNED");
                run_insert.number(6, millis(std::chrono::system_clock::now()));
                run_insert.run(); run_insert.reset();
                run_update.text(1, run_status_override.value_or(toString(task.getStatus())));
                run_update.number(2, millis(task.getStartedAt()));
                run_update.number(3, recovery ? millis(std::chrono::system_clock::now()) : millis(task.getFinishedAt()));
                run_update.nullable(4, task.getExecutionSummary().is_null() ? "" : task.getExecutionSummary().dump());
                run_update.text(5, task.getRunId());
                run_update.run(); run_update.reset();
            }
        }
        if (submission) {
            if (tasks.size() != 1) throw std::runtime_error("submission needs one task");
            submit.text(1, submission->first);
            submit.text(2, submission->second);
            submit.number(3, checkedId(tasks.front().getId()));
            submit.run();
        }
        if (recovery) {
            if (tasks.size() != 1) throw std::runtime_error("recovery needs one task");
            Statement decision(db_, "INSERT INTO recovery_decisions(run_id,task_id,worker_id,decision) VALUES(?,?,?,?)");
            decision.text(1, tasks.front().getRunId());
            decision.number(2, checkedId(tasks.front().getId()));
            decision.text(3, recovery->first);
            decision.text(4, recovery->second);
            decision.run();
        }
        exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
}
nlohmann::json StateStore::loadWorkerControls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT value FROM worker_management WHERE name='controls'");
    int step=sqlite3_step(query.value);
    if(step==SQLITE_DONE)return {{"machines",nlohmann::json::object()},{"workers",nlohmann::json::object()}};
    check(step,db_);return nlohmann::json::parse(column(query.value,0));
}
void StateStore::saveWorkerControls(const nlohmann::json& controls) {
    const auto serialized=controls.dump();
    std::lock_guard<std::mutex> lock(mutex_);
    Statement update(db_, "INSERT OR REPLACE INTO worker_management(name,value) VALUES('controls',?)");
    update.text(1,serialized);update.run();
}
} // namespace GCRP
