#include "EchoClient.h"
#include "Logging.h"
#include <sstream>
#include "protocol/message_codec.h"
#include "Utils.h"
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <unistd.h>

EchoClient::EchoClient(const std::string &host, int port)
    : host_(host), port_(port), sockfd_(-1) {}

EchoClient::~EchoClient() {
  this->stopHeartbeat();
  this->resetConnection();
}

bool EchoClient::connect() {

  if (this->state_ == ClientState::Connected && this->sockfd_ != -1) {
    return true;
  }

  this->resetConnection();
  this->state_ = ClientState::Connecting;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    this->state_ = ClientState::Disconnected;
    return false;
  }

  sockaddr_in serv_addr{};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(this->port_);
  if (inet_pton(AF_INET, this->host_.c_str(), &serv_addr.sin_addr) <= 0) {
    close(fd);
    this->state_ = ClientState::Disconnected;
    return false;
  }

  if (::connect(fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) == -1) {
    close(fd);
    this->state_ = ClientState::Disconnected;
    return false;
  }

  this->sockfd_ = fd;
  this->state_ = ClientState::Connected;
  this->reconnect_delay_ms_ = 1000;

  return true;
}

bool EchoClient::sendMessage(const std::string &msg, std::string &response) {
  std::lock_guard<std::mutex> lock(this->io_mutex_);

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!this->ensureConnected()) {
      return false;
    }

    auto encoded = MessageCodec::encode(msg);

    if (!this->writePacket(encoded)) {
      continue;
    }

    if (!this->readResponse(response)) {
      continue;
    }
    return true;
  }
  return false;
}

bool EchoClient::sendPing() {
  std::string resp;
  if (!this->sendMessage("__ping__", resp)) {
    return false;
  }
  return resp == "__pong__";
}

void EchoClient::startHeartbeat() {
  if (this->heartbeat_enabled_) {
    return;
  }
  this->running_ = true;
  this->heartbeat_enabled_ = true;
  this->heartbeat_thread_ = std::thread(&EchoClient::heartbeatLoop, this);
}

void EchoClient::stopHeartbeat() {
  this->running_ = false;
  this->heartbeat_enabled_ = false;

  this->cv_.notify_all();

  if (this->heartbeat_thread_.joinable()) {
    this->heartbeat_thread_.join();
  }
}

void EchoClient::disconnect() { this->markDisconnected(); }

void EchoClient::resetConnection() {
  if (this->sockfd_ != -1) {
    ::close(this->sockfd_);
    this->sockfd_ = -1;
  }
}

void EchoClient::markDisconnected() {
  bool had_connection =
      (this->sockfd_ != -1) || (this->state_ != ClientState::Disconnected);

  this->resetConnection();
  this->state_ = ClientState::Disconnected;
  this->inputBuffer_.retrieveAll();
  if (had_connection) {
    LOG_WARN(ForgeSched::LogModule::NETWORK, "client marked disconnected");
  }
}

bool EchoClient::ensureConnected() {
  if (this->state_ == ClientState::Connected && this->sockfd_ != -1) {
    return true;
  }
  return this->connect();
}

bool EchoClient::writePacket(const std::vector<char> &packet) {
  if (this->sockfd_ == -1) {
    return false;
  }
  if (!sendAll(this->sockfd_, packet.data(), packet.size())) {
    this->markDisconnected();
    return false;
  }
  return true;
}

bool EchoClient::readResponse(std::string &response) {
  char recv_buf[1024];

  while (true) {
    auto result =
        MessageCodec::Decoder::tryDecode(this->inputBuffer_, response);
    if (result == MessageCodec::DecodeResult::Ok) {
      return true;
    }

    if (result == MessageCodec::DecodeResult::Invalid) {
      this->markDisconnected();
      return false;
    }

    ssize_t n = ::recv(this->sockfd_, recv_buf, sizeof(recv_buf), 0);
    if (n == 0) {
      this->markDisconnected();
      return false;
    }

    if (n < 0) {
      this->markDisconnected();
      return false;
    }

    this->inputBuffer_.append(recv_buf, static_cast<size_t>(n));
  }
}

void EchoClient::heartbeatLoop() {
  std::unique_lock<std::mutex> cv_lock(this->cv_mutex_);

  while (this->running_) {
    if (this->cv_.wait_for(
            cv_lock, std::chrono::milliseconds(this->heartbeat_interval_ms_),
            [this]() { return !this->running_; })) {
      break;
    }

    cv_lock.unlock();

    if (!this->io_mutex_.try_lock()) {
      cv_lock.lock();
      continue;
    }

    std::unique_lock<std::mutex> io_lock(this->io_mutex_, std::adopt_lock);

    bool ok = this->sendPingUnlocked();
    if (ok) {
      this->heartbeat_fail_count_ = 0;
    } else {
      ++this->heartbeat_fail_count_;
      {
        std::ostringstream oss;
        oss << "heartbeat failed, count=" << this->heartbeat_fail_count_;
        LOG_WARN(ForgeSched::LogModule::NETWORK, oss.str());
      }

      if (this->heartbeat_fail_count_ >= this->heartbeat_fail_threshold_) {
        this->markDisconnected();
        this->heartbeat_fail_count_ = 0;
      }
    }
    io_lock.unlock();
    cv_lock.lock();
  }
}

bool EchoClient::sendPingUnlocked() {
  this->inputBuffer_.retrieveAll();
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!this->ensureConnected()) {
      return false;
    }

    std::string response;
    auto encoded = MessageCodec::encode("__ping__");

    LOG_INFO(ForgeSched::LogModule::NETWORK, "heartbeat ping sending");

    if (!this->writePacket(encoded)) {
      LOG_INFO(ForgeSched::LogModule::NETWORK, "heartbeat send failed");
      continue;
    }

    if (!this->readResponse(response)) {
      LOG_INFO(ForgeSched::LogModule::NETWORK, "heartbeat recv failed");
      continue;
    }

    LOG_INFO(ForgeSched::LogModule::NETWORK, "heartbeat pong received");
    return response == "__pong__";
  }

  {
    std::ostringstream oss;
    oss << "heartbeat failed, count=" << this->heartbeat_fail_count_;
    LOG_WARN(ForgeSched::LogModule::NETWORK, oss.str());
  }

  return false;
}
