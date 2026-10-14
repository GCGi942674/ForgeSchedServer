#include "Logging.h"
#include "TestSupport.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

int main() { return testMain([] {
  char path[] = "/tmp/gcrp-logging-XXXXXX";
  CHECK(::mkdtemp(path));
  struct Cleanup { const char* path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{path};
  GCRP::Logger::instance().setLogDirectory(path);
  GCRP::Logger::instance().setLevel(GCRP::LogLevel::DEBUG);

  LOG_DEBUG(GCRP::LogModule::SERVER, "debug message");
  LOG_INFO(GCRP::LogModule::SERVER, "info message");
  LOG_WARN(GCRP::LogModule::SERVER, "warn message");
  LOG_ERROR(GCRP::LogModule::SERVER, "error message");

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([i]() {
      for (int j = 0; j < 5; ++j) {
        {
          std::ostringstream oss;
          oss << "worker=" << i << ", j=" << j;
          LOG_INFO(GCRP::LogModule::SERVER, oss.str());
        }
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  GCRP::Logger::instance().setLevel(GCRP::LogLevel::WARN);
  LOG_INFO(GCRP::LogModule::SERVER, "FILTERED_MARKER");
  LOG_ERROR(GCRP::LogModule::TASK, "TASK_ERROR_MARKER");
  std::string server, task, errors;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream input(entry.path());
    std::string content((std::istreambuf_iterator<char>(input)), {});
    if (entry.path().filename() == "server.log") server += content;
    if (entry.path().filename() == "task.log") task += content;
    if (entry.path().filename() == "error.log") errors += content;
  }
  CHECK(server.find("[SERVER]") != std::string::npos);
  CHECK(server.find("FILTERED_MARKER") == std::string::npos);
  for (int i=0; i<4; ++i) for (int j=0; j<5; ++j)
    CHECK(server.find("worker=" + std::to_string(i) + ", j=" + std::to_string(j)) != std::string::npos);
  CHECK(task.find("[TASK]") != std::string::npos);
  CHECK(task.find("TASK_ERROR_MARKER") != std::string::npos);
  CHECK(errors.find("TASK_ERROR_MARKER") != std::string::npos);
  CHECK(errors.find("error message") != std::string::npos);
  CHECK(errors.find("info message") == std::string::npos);
  CHECK(std::string(GCRP::Logger::getBaseName("C:\\src\\test.cpp")) == "test.cpp");
  CHECK(std::string(GCRP::Logger::getBaseName("/src/test.cpp")) == "test.cpp");
  CHECK(std::string(GCRP::Logger::getBaseName(nullptr)).empty());
}); }
