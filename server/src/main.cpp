#include "EchoServer.h"
#include "WorkerServer.h"
#include "Logging.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "logger/LogLevel.h"
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <iostream>
#include <unordered_map>

static int g_signal_fd = -1;

void handle_sigint(int) {
  uint64_t one = 1;
  ssize_t n = ::write(g_signal_fd, &one, sizeof(one));
  (void)n;
}

ForgeSched::LogLevel parseLogLevel(const std::string& level_str) {
    static const std::unordered_map<std::string, ForgeSched::LogLevel> level_map = {
        {"DEBUG", ForgeSched::LogLevel::DEBUG},
        {"INFO", ForgeSched::LogLevel::INFO},
        {"WARN", ForgeSched::LogLevel::WARN},
        {"ERROR", ForgeSched::LogLevel::ERROR},
        {"FATAL", ForgeSched::LogLevel::FATAL}
    };

    auto it = level_map.find(level_str);
    if (it != level_map.end()) {
        return it->second;
    }
    std::cerr << "[CONFIG] Warning: Unknown log level '" << level_str << "'. Defaulting to INFO." << std::endl;
    return ForgeSched::LogLevel::INFO;
}

int main(int argc, char** argv) {
  // Step 1: Load Config
  if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--help")) {
    std::cerr << "Usage: forgesched_server [--help]\n";
    return 1;
  }
  if (argc == 2) {
    std::cout << "Set FORGESCHED_CONFIG to the shared config file path.\n";
    return 0;
  }
  bool config_loaded = ForgeSched::Config::instance().loadDefault();
  if (!config_loaded) {
      std::cerr << "[SERVER] Missing config; set FORGESCHED_CONFIG" << std::endl;
      return 1;
  }

  // Step 2: Configure Logger
  std::string log_level_str = ForgeSched::Config::instance().getString("log.level", "INFO");
  std::string log_dir = ForgeSched::Config::instance().getString("log.dir", "");

  ForgeSched::LogLevel log_level = parseLogLevel(log_level_str);
  ForgeSched::Logger::instance().setLogDirectory(log_dir);
  ForgeSched::Logger::instance().setLevel(log_level);

  // Step 3: Read server settings
  int port = ForgeSched::Config::instance().getInt("server.port", 0);
  int worker_threads = ForgeSched::Config::instance().getInt("server.worker_threads", 8);
  int io_threads = ForgeSched::Config::instance().getInt("server.io_threads", 2);
  const std::string bind_ip = ForgeSched::Config::instance().getString("server.bind_ip", "");
  if (port < 1 || port > 65535 || worker_threads < 1 || worker_threads > 256 ||
      io_threads < 1 || io_threads > 256 || log_dir.empty() || bind_ip.empty()) {
    std::cerr << "[SERVER] Invalid port or thread count" << std::endl;
    return 1;
  }

  std::cerr << "[SERVER] Configuration: port=" << port << ", workers=" << worker_threads
            << ", log_level=" << log_level_str << ", log_dir=" << log_dir << std::endl;

  LOG_INFO(ForgeSched::LogModule::SERVER, "server starting...");

  g_signal_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (g_signal_fd < 0) return 1;

  struct sigaction sa {};
  sa.sa_handler = handle_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Step 4: Start server with configured settings
  EchoHandler handler;
  ForgeSched::WorkerServer worker_server;
  EchoServer server(port, handler, g_signal_fd, io_threads, worker_threads, bind_ip);
  worker_server.attach(server);

  bool ran = server.run();

  if (g_signal_fd != -1) {
    ::close(g_signal_fd);
    g_signal_fd = -1;
  }

  LOG_INFO(ForgeSched::LogModule::SERVER, "server stopped!");

  return ran ? 0 : 1;
}
