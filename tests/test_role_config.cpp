#include "TestSupport.h"
#include "config/Config.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>

int main() { return testMain([] {
    const auto original = std::filesystem::current_path();
    const char* env = std::getenv("GCRP_CONFIG");
    const bool hadEnv = env != nullptr;
    const std::string saved = env ? env : "";
    char temporary[] = "/tmp/gcrp-role-config-XXXXXX";
    CHECK(::mkdtemp(temporary));
    const std::filesystem::path root(temporary);
    struct Restore {
        std::filesystem::path cwd, root; bool had; std::string env;
        ~Restore() { std::filesystem::current_path(cwd);
            if(had) ::setenv("GCRP_CONFIG",env.c_str(),1); else ::unsetenv("GCRP_CONFIG");
            std::error_code ec; std::filesystem::remove_all(root,ec); }
    } restore{original,root,hadEnv,saved};
    std::filesystem::create_directories(root/"config");
    std::filesystem::create_directories(root/"build"/"bin");
    {std::ofstream f(root/"config"/"gcrp.conf"); f<<"server.port=1234\n";}
    ::unsetenv("GCRP_CONFIG"); std::filesystem::current_path(root/"build"/"bin");
    auto& config=GCRP::Config::instance(); CHECK(config.loadDefault());
    CHECK(config.getInt("server.port",0)==1234);
    {std::ofstream f(root/"config"/"server.conf"); f<<"server.port=5678\n";}
    CHECK(config.loadDefault()); CHECK(config.getInt("server.port",0)==5678);
    ::setenv("GCRP_CONFIG",(root/"config"/"gcrp.conf").c_str(),1);
    CHECK(config.loadDefault()); CHECK(config.getInt("server.port",0)==1234);
    ::setenv("GCRP_CONFIG","",1); CHECK(!config.loadDefault());
    ::setenv("GCRP_CONFIG",(root/"missing.conf").c_str(),1); CHECK(!config.loadDefault());
}); }
