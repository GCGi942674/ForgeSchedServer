#include "EchoClient.h"
#include "EchoServer.h"
#include "Logging.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

static void stopServer(int signal_fd) {
  uint64_t one = 1;
  ::write(signal_fd, &one, sizeof(one));
}

static bool waitServerReady(int port) {
  for (int i = 0; i < 50; ++i) {
    EchoClient client("127.0.0.1", port);
    if (client.connect()) {
      client.disconnect();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

int main() {
  ForgeSched::Logger::instance().setLevel(ForgeSched::LogLevel::WARN);

  const int port = 18082;
  int signal_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  assert(signal_fd >= 0);

  EchoHandler handler;
  EchoServer server(port, handler, signal_fd, 4);

  std::thread server_thread([&server]() {
    server.run();
  });

  assert(waitServerReady(port));

  constexpr int kClientNum = 100;
  std::atomic<int> ok_count{0};
  std::vector<std::thread> clients;

  for (int i = 0; i < kClientNum; ++i) {
    clients.emplace_back([port, i, &ok_count]() {
      EchoClient client("127.0.0.1", port);

      if (!client.connect()) {
        return;
      }

      std::string msg = "msg-" + std::to_string(i);
      std::string response;

      if (client.sendMessage(msg, response) && response == msg) {
        ok_count.fetch_add(1);
      }

      client.disconnect();
    });
  }

  for (auto &t : clients) {
    t.join();
  }

  assert(ok_count.load() == kClientNum);

  stopServer(signal_fd);

  if (server_thread.joinable()) {
    server_thread.join();
  }

  ::close(signal_fd);

  std::cout << "test_EchoServer passed\n";
  return 0;
}
