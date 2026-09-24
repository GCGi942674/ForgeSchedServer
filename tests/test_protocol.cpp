#include "TestSupport.h"
#include "protocol/ProtocolCodec.h"
#include "protocol/ProtocolRouter.h"
#include "protocol/dto/WorkerDTO.h"
#include "config/Config.h"
#include <fstream>
#include <filesystem>
using namespace ForgeSched;
using namespace ForgeSched::Protocol;
int main() { return testMain([] {
    Buffer buffer; std::string decoded;
    auto packet = MessageCodec::encode("abc");
    buffer.append(packet.data(), 2);
    CHECK(MessageCodec::Decoder::tryDecode(buffer, decoded) == MessageCodec::DecodeResult::NeedMoreData);
    CHECK(buffer.readableBytes() == 2);
    buffer.append(packet.data()+2, 3);
    CHECK(MessageCodec::Decoder::tryDecode(buffer, decoded) == MessageCodec::DecodeResult::NeedMoreData);
    buffer.append(packet.data()+5, packet.size()-5);
    buffer.append(packet.data(), packet.size());
    for (int i=0; i<2; ++i) {
        CHECK(MessageCodec::Decoder::tryDecode(buffer, decoded) == MessageCodec::DecodeResult::Ok);
        CHECK(decoded == "abc");
    }
    CHECK(buffer.readableBytes() == 0);
    auto max = MessageCodec::encode(std::string(MessageCodec::kMaxBodyLenght, 'a'));
    buffer.append(max.data(), max.size());
    CHECK(MessageCodec::Decoder::tryDecode(buffer, decoded) == MessageCodec::DecodeResult::Ok);
    bool rejected = false;
    try { MessageCodec::encode(std::string(MessageCodec::kMaxBodyLenght+1, 'a')); }
    catch (const std::length_error&) { rejected = true; }
    CHECK(rejected);
    uint32_t oversized = htonl(MessageCodec::kMaxBodyLenght+1);
    buffer.append(reinterpret_cast<char*>(&oversized), 4);
    CHECK(MessageCodec::Decoder::tryDecode(buffer, decoded) == MessageCodec::DecodeResult::Invalid);
    ProtocolMessage message; ProtocolError error;
    auto valid = envelope("worker_heartbeat", {{"worker_id", "w"}});
    CHECK(ProtocolCodec::decode(valid.dump(), message, error));
    for (const auto& key : {"version", "type", "request_id", "data"}) {
        auto j = valid; j.erase(key); CHECK(!ProtocolCodec::decode(j.dump(), message, error));
    }
    for (auto bad : {nlohmann::json(-1), nlohmann::json(4294967297ULL), nlohmann::json("1")}) {
        auto j = valid; j["version"] = bad; CHECK(!ProtocolCodec::decode(j.dump(), message, error));
    }
    for (const char* bad : {"{", "[]", "null", "1"}) CHECK(!ProtocolCodec::decode(bad, message, error));
    DTO::WorkerRegisterRequest reg;
    for (auto slots : {0ULL, 4294967296ULL}) {
        CHECK(!DTO::fromJson(nlohmann::json{{"worker_id", "w"}, {"hostname", "h"}, {"slots", slots}}, reg, error));
    }
    WorkerManager workers; Scheduler scheduler(workers); TaskService service(scheduler);
    ProtocolRouter router(service, scheduler, workers);
    auto route = [&](const std::string& type, nlohmann::json data) {
        ProtocolMessage request;
        CHECK(ProtocolCodec::decode(envelope(type, std::move(data)).dump(), request, error));
        auto result = router.handle(request); CHECK(result.request_id == request.request_id); return result.data;
    };
    auto submit = nlohmann::json{{"task_type", "REGRESSION"}, {"target", "x"}, {"revision", "r"}};
    for (auto priority : {nlohmann::json(101), nlohmann::json(-101), nlohmann::json("1"), nlohmann::json(1.2)}) {
        auto bad = submit; bad["priority"] = priority; CHECK(route("submit_task", bad)["code"] != 0);
    }
    auto result = route("submit_task", submit); CHECK(result["code"] == 0);
    auto id = result.at("result").at("task_id");
    CHECK(route("query_task", {{"task_id", id}})["result"]["status"] == "QUEUED");
    CHECK(route("query_task", {{"task_id", 999999u}})["code"] != 0);
    CHECK(route("task_start", {{"task_id", id}, {"worker_id", "w"}})["code"] != 0);
    CHECK(route("cancel_task", {{"task_id", id}})["code"] == 0);
    CHECK(route("cancel_task", {{"task_id", id}})["code"] != 0);
    CHECK(route("response", nlohmann::json::object())["code"] != 0);
    // A unique fixture prevents parallel tests from changing each other's config.
    char path[] = "/tmp/forgesched-config-XXXXXX";
    Fd temp(::mkstemp(path)); CHECK(temp.value >= 0);
    struct Remove { const char* path; ~Remove() { ::unlink(path); } } cleanup{path};
    { std::ofstream out(path); CHECK(out.good());
      out << "# comment\n number = 42\nnumber=43\nbad=12junk\nbig=999999999999999999\nyes=YES\nno=off\ntext=a=b\nmalformed\n"; }
    auto& config = Config::instance(); CHECK(config.load(path));
    CHECK(config.getInt("number", 0) == 43); CHECK(config.getInt("bad", 7) == 7);
    CHECK(config.getInt("big", 7) == 7); CHECK(config.getBool("yes", false));
    CHECK(!config.getBool("no", true)); CHECK(config.getString("text", "") == "a=b");
    CHECK(!config.contains("missing"));
}); }
