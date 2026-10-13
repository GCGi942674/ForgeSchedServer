#include "ForgeClient.h"
#include "config/Config.h"
#include <arpa/inet.h>
#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
using namespace ForgeSched;
namespace {
struct UsageError : std::runtime_error { using std::runtime_error::runtime_error; };
template<class T> T number(const std::string& text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size())
        throw UsageError("invalid integer: " + text);
    return result;
}
void usage() {
    std::cout << "Usage: forgesched_client [--host IPv4] [--port PORT] [--timeout-ms MS]\n"
              << "Defaults come from FORGESCHED_CONFIG or config/forgesched.conf.\n"
              << "  submit --target TARGET --revision REV [--priority -100..100]\n"
              << "         [--case PATH/run.tcl --flow PROFILE --case-timeout SEC]\n"
              << "  query TASK_ID\n  cancel TASK_ID\n"
              << "  tasks [--status STATE] [--target TARGET] [--revision REV] [--batch-id ID] [--limit N] [--before ID]\n"
              << "  workers [--machine-id NAME] [--status STATE] [--limit N] [--after JSON_CURSOR]\n"
              << "  capabilities\n  status\n"
              << "  query-batch BATCH_ID\n  cancel-batch BATCH_ID\n"
              << "  regression --revision REV | --case PATH/run.tcl | --batch-id ID\n"
              << "             [--flow PROFILE] [--limit 1..50] [--before TASK_ID]\n"
              << "  progress --target TARGET [--flow PROFILE] [--limit 1..20] [--before-revision REV]\n"
              << "  rollback --target TARGET --from REV --to REV [--flow PROFILE]\n"
              << "           [--limit 1..50] [--after-task-id TASK_ID]\n"
              << "One JSON response on stdout; exit 0=OK, 2=server error, 1=transport/protocol error, 64=usage.\n"
              << "No automatic retries. Batch cancel stops queued cases; in-flight cases finish.\n";
}
}
int main(int argc, char** argv) {
    try {
        if (!Config::instance().loadDefault())
            throw UsageError("missing config; set FORGESCHED_CONFIG");
        std::string host = Config::instance().getString("network.server_ip", "");
        std::string command, target, revision, id, case_path, flow, batch_id;
        std::string from_revision, to_revision, before_revision;
        std::string status_filter, machine_filter, after_cursor;
        int port = Config::instance().getInt("server.port", 0);
        int timeout = Config::instance().getInt("network.timeout_ms", 0);
        int priority = 0, case_timeout = 0, limit = 0;
        uint64_t before = 0;
        uint64_t after_task_id = 0;
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") { usage(); return 0; }
            if (arg.rfind("--", 0) == 0) {
                if (!seen.insert(arg).second) throw UsageError("duplicate option: " + arg);
                if (++i >= argc) throw UsageError("missing value for " + arg);
                const std::string value = argv[i];
                if (arg == "--host") host = value;
                else if (arg == "--port") port = number<int>(value);
                else if (arg == "--timeout-ms") timeout = number<int>(value);
                else if (arg == "--priority") priority = number<int>(value);
                else if (arg == "--target") target = value;
                else if (arg == "--revision") revision = value;
                else if (arg == "--case") case_path = value;
                else if (arg == "--flow") flow = value;
                else if (arg == "--case-timeout") case_timeout = number<int>(value);
                else if (arg == "--batch-id") batch_id = value;
                else if (arg == "--limit") limit = number<int>(value);
                else if (arg == "--before") before = number<uint64_t>(value);
                else if (arg == "--before-revision") before_revision = value;
                else if (arg == "--from") from_revision = value;
                else if (arg == "--to") to_revision = value;
                else if (arg == "--after-task-id") after_task_id = number<uint64_t>(value);
                else if (arg == "--status") status_filter = value;
                else if (arg == "--machine-id") machine_filter = value;
                else if (arg == "--after") after_cursor = value;
                else throw UsageError("unknown option: " + arg);
            } else if (command.empty()) command = arg;
            else if (id.empty()) id = arg;
            else throw UsageError("unexpected argument: " + arg);
        }
        in_addr address{};
        if (::inet_pton(AF_INET, host.c_str(), &address) != 1)
            throw UsageError("--host must be a numeric IPv4 address");
        if (port < 1 || port > 65535 || timeout < 1 || timeout > 60000)
            throw UsageError("port or timeout out of range");
        if (priority < -100 || priority > 100) throw UsageError("priority out of range");
        ForgeClient client(host, static_cast<uint16_t>(port), timeout);
        Protocol::ProtocolMessage response;
        if (command != "tasks" && command != "workers" &&
            (seen.count("--status") || seen.count("--machine-id") || seen.count("--after")))
            throw UsageError("list filters require tasks or workers");
        if (command == "tasks" || command == "workers" || command == "capabilities" || command == "status") {
            std::set<std::string> allowed{"--host", "--port", "--timeout-ms"};
            if (command == "tasks") allowed.insert({"--status", "--target", "--revision", "--batch-id", "--limit", "--before"});
            if (command == "workers") allowed.insert({"--status", "--machine-id", "--limit", "--after"});
            for (const auto& option : seen)
                if (!allowed.count(option)) throw UsageError("unsupported list option: " + option);
            if (!id.empty()) throw UsageError("unexpected positional argument");
            nlohmann::json q = nlohmann::json::object();
            if (seen.count("--limit")) q["limit"] = limit;
            if (seen.count("--status")) q["status"] = status_filter;
            if (seen.count("--target")) q["target"] = target;
            if (seen.count("--revision")) q["revision"] = revision;
            if (seen.count("--batch-id")) q["batch_id"] = batch_id;
            if (seen.count("--before")) q["before_task_id"] = before;
            if (seen.count("--machine-id")) q["machine_id"] = machine_filter;
            if (seen.count("--after")) {
                q["after"] = nlohmann::json::parse(after_cursor, nullptr, false);
                if (q["after"].is_discarded()) throw UsageError("invalid cursor JSON");
            }
            auto type = command == "tasks" ? Protocol::MessageType::QUERY_TASKS :
                command == "workers" ? Protocol::MessageType::QUERY_WORKERS :
                command == "capabilities" ? Protocol::MessageType::QUERY_CAPABILITIES : Protocol::MessageType::QUERY_STATUS;
            response = client.request(type, q);
        } else if (command == "submit") {
            if (seen.count("--batch-id") || seen.count("--limit") || seen.count("--before") ||
                seen.count("--before-revision") || seen.count("--from") || seen.count("--to") ||
                seen.count("--after-task-id"))
                throw UsageError("regression query options are not allowed for submit");
            if (target.empty() || revision.empty() || !id.empty())
                throw UsageError("submit requires --target and --revision, with no positional arguments");
            if (seen.count("--case") || seen.count("--flow") || seen.count("--case-timeout")) {
                if (case_path.empty() || flow.empty() || case_timeout < 1 || case_timeout > 86400 ||
                    !seen.count("--case") || !seen.count("--flow") || !seen.count("--case-timeout"))
                    throw UsageError("PJtest submit requires --case, --flow and --case-timeout");
                response = client.request(Protocol::MessageType::SUBMIT_TASK,
                    {{"task_type", "REGRESSION"}, {"target", target}, {"revision", revision},
                     {"priority", priority}, {"payload", {{"spec_version", 1},
                        {"case", case_path}, {"flow", flow}, {"timeout_seconds", case_timeout}}}});
            } else response = client.submit(target, revision, priority);
        } else if (command == "regression") {
            if (!id.empty() || seen.count("--target") || seen.count("--priority") ||
                seen.count("--case-timeout") ||
                seen.count("--before-revision") || seen.count("--from") ||
                seen.count("--to") || seen.count("--after-task-id") ||
                (revision.empty() && case_path.empty() && batch_id.empty()) ||
                (seen.count("--limit") && (limit < 1 || limit > 50)) ||
                (seen.count("--before") && before == 0))
                throw UsageError("invalid regression query options");
            nlohmann::json filter = nlohmann::json::object();
            if (!revision.empty()) filter["revision"] = revision;
            if (!case_path.empty()) filter["case"] = case_path;
            if (!flow.empty()) filter["flow"] = flow;
            if (!batch_id.empty()) filter["batch_id"] = batch_id;
            if (seen.count("--limit")) filter["limit"] = limit;
            if (seen.count("--before")) filter["before_task_id"] = before;
            response = client.queryRegression(filter);
        } else if (command == "progress" || command == "rollback") {
            if (!id.empty() || target.empty() || seen.count("--priority") ||
                seen.count("--revision") || seen.count("--case") ||
                seen.count("--case-timeout") || seen.count("--batch-id") ||
                seen.count("--before") ||
                (seen.count("--limit") && (limit < 1 ||
                    limit > (command == "progress" ? 20 : 50))))
                throw UsageError("invalid progress/rollback query options");
            nlohmann::json filter = {{"target", target}};
            if (!flow.empty()) filter["flow"] = flow;
            if (seen.count("--limit")) filter["limit"] = limit;
            if (command == "progress") {
                if (seen.count("--from") || seen.count("--to") || seen.count("--after-task-id"))
                    throw UsageError("invalid progress query options");
                if (seen.count("--before-revision")) filter["before_revision"] = before_revision;
                response = client.queryProgress(filter);
            } else {
                if (seen.count("--before-revision") || from_revision.empty() ||
                    to_revision.empty() || (seen.count("--after-task-id") && !after_task_id))
                    throw UsageError("rollback requires --from and --to");
                filter["from_revision"] = from_revision;
                filter["to_revision"] = to_revision;
                if (seen.count("--after-task-id")) filter["after_task_id"] = after_task_id;
                response = client.queryRollback(filter);
            }
        } else if (command == "query" || command == "cancel" ||
                   command == "query-batch" || command == "cancel-batch") {
            if (seen.count("--target") || seen.count("--revision") || seen.count("--priority") ||
                seen.count("--case") || seen.count("--flow") || seen.count("--case-timeout") ||
                seen.count("--batch-id") || seen.count("--limit") || seen.count("--before") ||
                seen.count("--before-revision") || seen.count("--from") ||
                seen.count("--to") || seen.count("--after-task-id"))
                throw UsageError("submit options are not allowed for query/cancel");
            if (command == "query-batch" || command == "cancel-batch") {
                if (id.empty() || id.size() > 128) throw UsageError("invalid batch id");
                response = command == "query-batch" ? client.queryBatch(id) : client.cancelBatch(id);
            } else {
                auto task_id = number<uint64_t>(id);
                if (!task_id) throw UsageError("task id must be positive");
                response = command == "query" ? client.query(task_id) : client.cancel(task_id);
            }
        } else throw UsageError("expected submit, query, cancel, query-batch, cancel-batch, regression, progress or rollback (see --help)");
        std::cout << response.data.dump() << '\n';
        return response.data["code"] == 0 ? 0 : 2;
    } catch (const UsageError& e) {
        std::cerr << e.what() << '\n'; return 64;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nNo automatic retry; a mutation may already have been accepted.\n";
        return 1;
    }
}
