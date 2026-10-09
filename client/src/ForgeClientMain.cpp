#include "ForgeClient.h"
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
    std::cout << "Usage: forgesched_client [--host IPv4] [--port 8080] [--timeout-ms 5000]\n"
              << "  submit --target TARGET --revision REV [--priority -100..100]\n"
              << "         [--case PATH/run.tcl --flow PROFILE --case-timeout SEC]\n"
              << "  query TASK_ID\n  cancel TASK_ID\n"
              << "One JSON response on stdout; exit 0=OK, 2=server error, 1=transport/protocol error, 64=usage.\n"
              << "No automatic retries. Cancel is local state cancellation, not remote process termination.\n";
}
}
int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1", command, target, revision, id, case_path, flow;
        int port = 8080, timeout = 5000, priority = 0, case_timeout = 0;
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
        if (command == "submit") {
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
        } else if (command == "query" || command == "cancel") {
            if (seen.count("--target") || seen.count("--revision") || seen.count("--priority") ||
                seen.count("--case") || seen.count("--flow") || seen.count("--case-timeout"))
                throw UsageError("submit options are not allowed for query/cancel");
            auto task_id = number<uint64_t>(id);
            if (!task_id) throw UsageError("task id must be positive");
            response = command == "query" ? client.query(task_id) : client.cancel(task_id);
        } else throw UsageError("expected submit, query or cancel (see --help)");
        std::cout << response.data.dump() << '\n';
        return response.data["code"] == 0 ? 0 : 2;
    } catch (const UsageError& e) {
        std::cerr << e.what() << '\n'; return 64;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nNo automatic retry; a mutation may already have been accepted.\n";
        return 1;
    }
}
