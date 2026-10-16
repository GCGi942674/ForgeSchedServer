#include "TestSupport.h"
#include "protocol/GuiCatalog.h"
#include "protocol/ConnectionRole.h"
#include "config/Config.h"
#include <filesystem>
#include <fstream>
using namespace GCRP;
using Json=nlohmann::json;
namespace fs=std::filesystem;
int main(){return testMain([]{
    char temporary[]="/tmp/gcrp-gui-catalog-XXXXXX";CHECK(::mkdtemp(temporary));
    const fs::path base(temporary);
    struct Cleanup{fs::path root;~Cleanup(){std::error_code e;fs::remove_all(root,e);}} cleanup{base};
    const auto root=base/"test2",templates=base/"templates",archives=base/"zip";
    fs::create_directories(root);fs::create_directory(templates);fs::create_directory(archives);
    for(int i=0;i<405;++i){auto path=root/("device/case"+std::to_string(i));fs::create_directories(path);std::ofstream(path/"run.tcl")<<"# case";}
    fs::create_directories(root/".svn");std::ofstream(root/".svn/run.tcl")<<"# exclude";
    fs::create_directories(root/"vivado_runner/runtime");std::ofstream(root/"vivado_runner/runtime/run.tcl")<<"# exclude";
    fs::create_directories(base/"outside");std::ofstream(base/"outside/run.tcl")<<"# exclude";
    fs::create_directory_symlink(base/"outside",root/"link");
    std::ofstream(templates/"route.json")<<R"({"flow_config":{"route_design":1,"bit_cmp":0}})";
    std::ofstream(templates/"broken.json")<<"invalid json";
    for(int i=1;i<=65;++i)std::ofstream(archives/("GalaxCore_"+std::to_string(i)+".zip"))<<"fixture";
    std::ofstream(archives/"GalaxCore_fake.zip")<<"excluded";
    std::ofstream(base/"protected")<<"1\n999\n";
    auto configure=[&](bool missing=false){std::ofstream f(base/"config");f<<"server.work_root="<<(missing?base/"missing":root).string()<<"\nserver.templates_dir="<<templates.string()<<"\nserver.artifact_root="<<archives.string()<<"\nserver.protect_versions_file="<<(base/"protected").string()<<"\n";f.close();CHECK(Config::instance().load((base/"config").string()));};
    configure();GuiCatalog catalog;
    CHECK(Protocol::isAllowed(Protocol::ConnectionRole::CLIENT,Protocol::MessageType::QUERY_CATALOG));
    CHECK(!Protocol::isAllowed(Protocol::ConnectionRole::WORKER,Protocol::MessageType::QUERY_CATALOG));
    auto page=catalog.query(Json::object());CHECK(page["total_cases"]==405);CHECK(page["cases"].size()==200);
    CHECK(page["revisions"].size()==51);CHECK(page["revisions"][0]=="65");CHECK(page["revisions"].back()=="1");
    CHECK(page["protected_revisions"]==Json::array({"1"}));CHECK(page["templates"].size()==1);
    CHECK(page["templates"]["route"]["flow_config"]["route_design"]==1);
    CHECK(!page["warnings"].empty());CHECK(page["sources"]["server.work_root"]=="READY");
    const auto generation=page["snapshot_generation"];
    auto next=catalog.query({{"offset",page["next_offset"]},{"snapshot_generation",generation}});
    CHECK(next["cases"].size()==200);CHECK(!next.contains("templates"));
    next=catalog.query({{"offset",next["next_offset"]},{"snapshot_generation",generation}});
    CHECK(next["cases"].size()==5);CHECK(next["next_offset"].is_null());
    for(const auto& q:{Json{{"path","/etc"}},Json{{"offset",-1}},Json{{"offset",true}},Json{{"offset",1}},Json{{"refresh",1}}}){
        bool rejected=false;try{catalog.query(q);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    }
    CHECK(catalog.query({{"refresh",true}})["snapshot_generation"]!=generation);
    CHECK(catalog.query({{"offset",200},{"snapshot_generation",generation}})["cases"].size()==200);
    for(int i=0;i<3;++i)catalog.query({{"refresh",true}});
    bool changed=false;try{catalog.query({{"offset",200},{"snapshot_generation",generation}});}catch(const std::logic_error&){changed=true;}CHECK(changed);
    configure(true);page=catalog.query({{"refresh",true}});CHECK(page["cases"].empty());
    CHECK(page["sources"]["server.work_root"]=="ERROR");CHECK(page["templates"].size()==1);CHECK(page["revisions"].size()==51);
});}
