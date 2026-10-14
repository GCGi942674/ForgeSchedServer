#include "ControlTestSupport.h"
using namespace GCRP;
int main(){return testMain([]{
    ControlTestServer host;auto client=connectTo(host.port);
    auto submit=[&](const std::string& day,const std::string& stage,const std::string& source){
        const auto answer=exchangeJson(client.value,envelope("submit_task",{
            {"task_type","REGRESSION"},{"target","device"},{"revision","123"},
            {"payload",{{"spec_version",2},{"case","cases/a/run.tcl"},{"flow",stage},
                {"timeout_seconds",uint64_t(60)},{"flow_config",{{"route_design",1}}},
                {"context",{{"source",source},{"date",day},{"template",stage},
                    {"suite","daily"},{"name",stage},{"batch_id",day+stage+source}}}}}}));
        CHECK(answer["code"]==0);return answer["result"]["task_id"].get<uint64_t>();
    };
    const auto a=submit("2026-10-11","route_design","scheduled");
    const auto b=submit("2026-10-12","route_design","scheduled");
    submit("2026-10-12","place_design","scheduled");
    submit("2026-10-12","route_design","manual");
    auto request=nlohmann::json{{"baseline_date","2026-10-11"},{"date","2026-10-12"},{"limit",1}};
    auto first=exchangeJson(client.value,envelope("query_regression",request));
    CHECK(first["code"]==0);CHECK(first["result"]["total"]==2);
    CHECK(first["result"]["items"].size()==1&&first["result"]["has_more"]==true);
    request["after_key"]=first["result"]["next_after_key"];
    auto second=exchangeJson(client.value,envelope("query_regression",request));
    CHECK(second["code"]==0&&second["result"]["has_more"]==false);
    CHECK(second["result"]["items"][0]["baseline"]["task_id"]==a);
    CHECK(second["result"]["items"][0]["comparison"]["task_id"]==b);
    request.erase("after_key");request["template"]="route_design";
    CHECK(exchangeJson(client.value,envelope("query_regression",request))["result"]["total"]==1);
    request["date"]="2026-02-30";
    CHECK(exchangeJson(client.value,envelope("query_regression",request))["code"]==1);
});}
