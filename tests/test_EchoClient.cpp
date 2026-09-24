#include "EchoClient.h"
#include "EchoServer.h"
#include "Logging.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

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

  const int port = 18081;
  int signal_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  assert(signal_fd >= 0);

  EchoHandler handler;
  EchoServer server(port, handler, signal_fd, 2);

  std::thread server_thread([&server]() {
    server.run();
  });

  assert(waitServerReady(port));

  EchoClient client("127.0.0.1", port);
  assert(client.connect());

  std::string response;

  assert(client.sendMessage("hello", response));
  assert(response == "hello");

  assert(client.sendMessage("abc123", response));
  assert(response == "abc123");

  assert(client.sendPing());

  client.disconnect();

  stopServer(signal_fd);

  if (server_thread.joinable()) {
    server_thread.join();
  }

  ::close(signal_fd);

  std::cout << "test_EchoClient passed\n";
  return 0;
}
