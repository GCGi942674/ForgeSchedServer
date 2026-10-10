#include "TestSupport.h"
#include "config/Config.h"
#include "protocol/ProtocolRouter.h"
#include "scheduler/SchedulingCoordinator.h"
#include "scheduler/SchedulingDriver.h"
#include "task/TaskService.h"
#include "task/RegressionPayload.h"
#include "worker/WorkerDispatcher.h"
#include <filesystem>
#include <fstream>
using namespace ForgeSched;
struct FlowDispatcher : WorkerDispatcher {
    bool dispatch(const WorkerId&, const Task&) override { return false; }
};
int main() { return testMain([] {
    char name[] = "/tmp/forge-flow-XXXXXX";
    CHECK(::mkdtemp(name));
    const std::filesystem::path root(name), templates = root / "templates";
    struct Remove {
        std::filesystem::path path;
        ~Remove() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::filesystem::create_directories(templates);
    { std::ofstream config(root / "config"); config << "server.templates_dir=" << templates.string() << "\n"; }
    CHECK(Config::instance().load((root / "config").string()));
    CHECK(Config::instance().getString("server.templates_dir", "") == templates.string());
    auto writeTemplate = [&](int stage) {
        std::ofstream file(templates / "route.json");
        file << "{\"flow_config\":{\"route_design\":" << stage << ",\"write_bitstream\":1}}";
    };
    WorkerManager workers;
    Scheduler scheduler(workers);
    FlowDispatcher dispatcher;
    SchedulingCoordinator coordinator(scheduler, dispatcher);
    SchedulingDriver driver(coordinator);
    TaskService service(scheduler, driver);
    Protocol::ProtocolRouter router(service, scheduler, workers, driver);
    CreateTaskRequest request{TaskType::REGRESSION, "target", "18920", 0,
        {{"spec_version", 1}, {"case", "case/run.tcl"}, {"flow", "route"},
         {"timeout_seconds", 600u}}};
    CHECK(validRegressionPayload(request.payload));
    writeTemplate(1);
    { std::ifstream stream(templates / "route.json");
      const auto source = nlohmann::json::parse(stream, nullptr, false);
      CHECK(source.is_object());
      CHECK(validFlowConfig(source["flow_config"]));
      auto upgraded = request.payload;
      upgraded["spec_version"] = 2;
      upgraded["flow_config"] = source["flow_config"];
      CHECK(validRegressionPayload(upgraded)); }
    auto first = service.createTask(request); CHECK(first);
    writeTemplate(0);
    auto second = service.createTask(request); CHECK(second);
    CHECK(service.getTask(*first)->getPayload()["spec_version"] == 2);
    CHECK(service.getTask(*first)->getPayload()["flow_config"]["route_design"] == 1);
    CHECK(service.getTask(*second)->getPayload()["flow_config"]["route_design"] == 0);
    auto invalid = request;
    invalid.payload["spec_version"] = 2;
    invalid.payload["flow_config"] = {{"route_design", "1;exec"}};
    CHECK(!service.createTask(invalid));
    Protocol::ProtocolMessage query;
    query.version = 1;
    query.type = Protocol::MessageType::QUERY_STATUS;
    query.request_id = 1;
    query.data = nlohmann::json::object();
    const auto reply = router.handle(query);
    CHECK(reply.data["code"] == 0);
    CHECK(reply.data["result"]["queued"] == 2);
    CHECK(reply.data["result"]["total"] == 2);
}); }
