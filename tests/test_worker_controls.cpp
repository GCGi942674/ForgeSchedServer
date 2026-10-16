#include "TestSupport.h"
#include "worker/WorkerManager.h"
#include "persistence/StateStore.h"
#include "protocol/ConnectionRole.h"
#include <filesystem>
#include <sqlite3.h>
using namespace GCRP;
using Json=nlohmann::json;

int main(){return testMain([]{
    CHECK(Protocol::isAllowed(Protocol::ConnectionRole::CLIENT,Protocol::MessageType::CONTROL_WORKERS));
    CHECK(!Protocol::isAllowed(Protocol::ConnectionRole::WORKER,Protocol::MessageType::CONTROL_WORKERS));
    char dir[]="/tmp/gcrp-worker-controls-XXXXXX";CHECK(::mkdtemp(dir));
    struct Cleanup{const char* path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{dir};
    const auto database=(std::filesystem::path(dir)/"state.db").string();
    {
        StateStore store(database);WorkerManager workers(&store);
        for(const auto& id:{"h-slot1","h-slot2"}){
            CHECK(workers.registerWorker(id,"h",1,"h",id));workers.setControlSupported(id,true);}
        CHECK(workers.registerWorker("other-slot1","other",1,"other","slot1"));
        CHECK(workers.acquireSlot("h-slot1"));
        const auto paused=workers.control(true,"h","pause","pause-host","");CHECK(paused);
        CHECK(workers.getWorker("h-slot1")->getUsedSlots()==1);
        CHECK(workers.heartbeat("h-slot1"));CHECK(!workers.getWorker("h-slot1")->acceptsTasks());
        CHECK(!workers.acquireSlot("h-slot2"));CHECK(workers.getAvailableWorkers().size()==1);
        CHECK(workers.control(true,"h","pause","pause-host","")==paused);
        bool rejected=false;try{workers.control(true,"h","resume","pause-host","");}
        catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
        CHECK(workers.registerWorker("h-slot3","h",1,"h","slot3"));
        CHECK(!workers.getWorker("h-slot3")->acceptsTasks()); // Newly added slots inherit host policy.
        CHECK(workers.control(false,"h-slot2","resume","resume-slot","").has_value());
        CHECK(workers.getWorker("h-slot2")->acceptsTasks());CHECK(!workers.getWorker("h-slot1")->acceptsTasks());
        CHECK(workers.control(true,"h","resume","resume-host","").has_value());
        CHECK(workers.control(false,"h-slot1","update","update-active","test2").has_value());
        CHECK(workers.managementHeartbeat("h-slot1",nullptr)["command"].is_null());
        CHECK(!workers.acquireSlot("h-slot1"));CHECK(workers.releaseSlot("h-slot1"));
        CHECK(workers.managementHeartbeat("h-slot1",nullptr)["command"]["operation_id"]=="update-active");
        auto ack=Json{{"operation_id","update-active"},{"state","RUNNING"},{"error",""}};
        CHECK(workers.managementHeartbeat("h-slot1",ack)["maintenance_state"]=="RUNNING");
        rejected=false;try{workers.control(false,"h-slot1","resume","too-early","");}
        catch(const std::logic_error&){rejected=true;}CHECK(rejected);
        CHECK(workers.control(false,"h-slot1","pause","pause-during-update","").has_value());
        ack["state"]="SUCCEEDED";CHECK(workers.managementHeartbeat("h-slot1",ack)["accepting_tasks"]==false);
        CHECK(workers.control(false,"h-slot2","update","update-fail","all").has_value());
        auto failed=Json{{"operation_id","update-fail"},{"state","FAILED"},{"error","SVN conflict"}};
        CHECK(workers.managementHeartbeat("h-slot2",failed)["accepting_tasks"]==false);
        CHECK(workers.registerWorker("h-slot2","h",1,"h","h-slot2"));
        CHECK(!workers.getWorker("h-slot2")->acceptsTasks());
        CHECK(workers.control(false,"h-slot2","resume","resume-failed","").has_value());
        CHECK(workers.managementHeartbeat("h-slot2",failed)["accepting_tasks"]==true); // Old ACK is harmless.
        CHECK(workers.markOffline("h-slot3"));
        rejected=false;try{workers.control(true,"h","update","bulk-rejected","all");}
        catch(const std::logic_error&){rejected=true;}CHECK(rejected);
        CHECK(workers.getWorker("h-slot2")->getControl().state=="IDLE"); // Bulk operation is atomic.
        sqlite3* db=nullptr;CHECK(sqlite3_open(database.c_str(),&db)==SQLITE_OK);
        CHECK(sqlite3_exec(db,"CREATE TRIGGER reject_control BEFORE INSERT ON worker_management BEGIN SELECT RAISE(ABORT,'fixture'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
        rejected=false;try{workers.control(false,"h-slot2","pause","persist-fail","");}
        catch(const std::runtime_error&){rejected=true;}CHECK(rejected);
        CHECK(workers.getWorker("h-slot2")->acceptsTasks());
        CHECK(sqlite3_exec(db,"DROP TRIGGER reject_control",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
        CHECK(workers.control(false,"h-slot2","pause","persist-fail","").has_value());
        CHECK(workers.control(false,"h-slot1","update","pending-restart","all").has_value());
        for(const auto& slot:{"online","offline","draining"})CHECK(workers.registerWorker(std::string("resume-")+slot,"resume",1,"resume",slot));
        CHECK(workers.control(true,"resume","pause","pause-resume-fixture","").has_value());
        CHECK(workers.markOffline("resume-offline"));CHECK(workers.markDraining("resume-draining"));
        const auto resumed=workers.control(true,"resume","resume","resume-online-only","");
        CHECK(resumed&&(*resumed)["accepted"]==1);CHECK((*resumed)["worker_ids"]==Json::array({"resume-online"}));
        CHECK((*resumed)["skipped_worker_ids"].size()==2);
        CHECK(workers.getWorker("resume-online")->acceptsTasks());
        CHECK(!workers.getWorker("resume-offline")->getControl().accepting);
        CHECK(!workers.getWorker("resume-draining")->getControl().accepting);
        CHECK(workers.getWorker("resume-offline")->getStatus()==WorkerStatus::OFFLINE);
        rejected=false;try{workers.control(false,"resume-offline","resume","offline-direct","");}catch(const std::logic_error&){rejected=true;}CHECK(rejected);
        CHECK(workers.registerWorker("resume-future","resume",1,"resume","future"));
        CHECK(!workers.getWorker("resume-future")->getControl().accepting);
    }
    {
        StateStore store(database);WorkerManager restored(&store);
        CHECK(restored.registerWorker("h-slot1","h",1,"h","h-slot1"));
        CHECK(restored.registerWorker("h-slot2","h",1,"h","h-slot2"));
        CHECK(!restored.getWorker("h-slot1")->acceptsTasks());
        CHECK(!restored.getWorker("h-slot2")->acceptsTasks());
        CHECK(restored.getWorker("h-slot1")->getControl().operation_id=="pending-restart");
        CHECK(restored.managementHeartbeat("h-slot1",nullptr)["command"]["scope"]=="all");
        CHECK(restored.control(false,"h-slot2","pause","persist-fail","").has_value());
        CHECK(restored.registerWorker("resume-offline","resume",1,"resume","offline"));
        CHECK(!restored.getWorker("resume-offline")->getControl().accepting);
        CHECK(restored.registerWorker("resume-online","resume",1,"resume","online"));
        CHECK(restored.getWorker("resume-online")->getControl().accepting);
    }
});}
