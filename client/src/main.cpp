#include "EchoClient.h"
#include "Logging.h"
#include "config/Config.h"
#include <atomic>
#include <iostream>
#include <string>
#include <thread>

using namespace std;

int main() {
  if (!ForgeSched::Config::instance().loadDefault()) {
    cerr << "Missing config; set FORGESCHED_CONFIG" << endl;
    return 1;
  }
  const auto ip = ForgeSched::Config::instance().getString("network.server_ip", "");
  const auto port = ForgeSched::Config::instance().getInt("server.port", 0);
  if (ip.empty() || port < 1 || port > 65535) return 1;
  EchoClient client(ip, port);
  if (!client.connect()) {
    cerr << "Failed to connect to server." << endl;
    return 1;
  }
  client.startHeartbeat();

  string input;
  while (true) {
    cout << "Client: ";
    getline(cin, input);
    if (input == "exit")
      break;

    string response;
    if (!client.sendMessage(input, response)) {
      cerr << "Communication error." << endl;
      break;
    }
    cout << "Server: " << response << endl;
  }

  client.disconnect();
  return 0;
}
