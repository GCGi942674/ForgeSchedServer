#include "Connection.h"
#include "Logging.h"
#include "Utils.h"
#include <cstring>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace ForgeSched;

Connection::Connection(EventLoop *loop, int fd)
    : owner_loop_(loop), fd_(fd), state_(ConnState::Connected) {
  this->refreshActivity();
  std::ostringstream oss;
  oss << "connection created, fd=" << this->fd_;
  LOG_DEBUG(LogModule::NETWORK, oss.str());
}

EventLoop *Connection::ownerLoop() const { return this->owner_loop_; }

Connection::~Connection() {
  if (this->fd_ != -1) {
    close(this->fd_);
    this->fd_ = -1;
  }
}

int Connection::fd() const { return fd_; }

Connection::ReadResult Connection::handleRead() {
  ReadResult read_res;

  char buffer[4096];

  while (true) {
    ssize_t n = recv(this->fd_, buffer, sizeof(buffer), 0);

    if (n > 0) {
      this->refreshActivity();
      std::ostringstream oss;
      oss << "recv success, fd=" << this->fd_ << ", bytes=" << n;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
      this->inputBuffer_.append(buffer, static_cast<size_t>(n));

      read_res.bytes_received += n;

    } else if (n == 0) {
      std::ostringstream oss;
      oss << "peer closed connection, fd=" << this->fd_;
      LOG_INFO(LogModule::NETWORK, oss.str());
      this->shutdown();
      read_res.peer_close = true;
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      std::ostringstream oss;
      oss << "recv failed, fd=" << this->fd_ << ", errno=" << errno << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
      this->setState(ConnState::Disconnected);
      read_res.ok = false;
      return read_res;
    }
  }
  std::string msg;
  while (true) {
    auto result = MessageCodec::Decoder::tryDecode(this->inputBuffer_, msg);

    if (result == MessageCodec::DecodeResult::Ok) {
      std::ostringstream oss;
      oss << "message decoded, fd=" << this->fd_ << ", msg_size=" << msg.size();
      LOG_DEBUG(LogModule::NETWORK, oss.str());
      read_res.messages_decoded++;
      if (msg == "__ping__") {
        read_res.heartbeat_messages++;
      }

      if (this->on_message_) {
        this->incPendingTasks();
        this->on_message_(this->shared_from_this(), msg);

      } else {
        std::ostringstream oss;
        oss << "message callback not set, fd=" << this->fd_;
        LOG_WARN(LogModule::NETWORK, oss.str());
      }
      continue;
    }

    if (result == MessageCodec::DecodeResult::NeedMoreData) {
      std::ostringstream oss;
      oss << "decode need more data, fd=" << this->fd_;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
      break;
    }

    std::ostringstream oss;
    oss << "invalid packet, fd=" << this->fd_;
    LOG_WARN(LogModule::NETWORK, oss.str());
    this->setState(ConnState::Disconnected);
    read_res.ok = false;
    read_res.decode_error = true;
    return read_res;
  }

  return read_res;
}

Connection::WriteResult Connection::handleWrite() {
  WriteResult write_res;

  while (this->outputBuffer_.readableBytes() > 0) {

    ssize_t n = ::send(this->fd_, this->outputBuffer_.peek(),
                       this->outputBuffer_.readableBytes(), MSG_NOSIGNAL);
    if (n > 0) {
      this->refreshActivity();
      std::ostringstream oss;
      oss << "send success, fd=" << this->fd_ << ", bytes=" << n;
      LOG_DEBUG(LogModule::NETWORK, oss.str());
      this->outputBuffer_.retrieve(static_cast<size_t>(n));
      write_res.bytes_sent += n;

    } else if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::ostringstream oss;
        oss << "send would block, fd=" << this->fd_;
        LOG_DEBUG(LogModule::NETWORK, oss.str());
        return write_res;
      }
      std::ostringstream oss;
      oss << "send failed, fd=" << this->fd_ << ", errno=" << errno << ", err=" << strerror(errno);
      LOG_ERROR(LogModule::NETWORK, oss.str());
      this->setState(ConnState::Disconnected);
      write_res.ok = false;
      write_res.close = true;
      return write_res;
    } else {
      std::ostringstream oss;
      oss << "send returned 0, fd=" << this->fd_;
      LOG_WARN(LogModule::NETWORK, oss.str());
      this->setState(ConnState::Disconnected);
      write_res.ok = false;
      write_res.close = true;
      return write_res;
    }
  }

  if (this->canBeClosed()) {
    std::ostringstream oss;
    oss << "write buffer drained, closing disconnecting connection, fd=" << this->fd_;
    LOG_INFO(LogModule::NETWORK, oss.str());
    this->setState(ConnState::Disconnected);
    write_res.close = true;
    return write_res;
  }

  std::ostringstream oss;
  oss << "write buffer drained, fd=" << this->fd_;
  LOG_DEBUG(LogModule::NETWORK, oss.str());
  return write_res;
}

void Connection::setMessageCallback(MessageCallback cb) {
  this->on_message_ = std::move(cb);
}

void Connection::setWriteReadyCallback(
    std::function<void(const std::shared_ptr<Connection>&)> cb) {
  on_write_ready_ = std::move(cb);
}

bool Connection::sendPacket(const std::vector<char> &packet) {
  if (isDisconnected() || owner_loop_ == nullptr || packet.empty()) {
    return false;
  }
  auto weak = weak_from_this();
  if (weak.expired()) return false;
  ++pending_packets_;
  try {
    if (owner_loop_->tryQueueInLoop([weak, packet]() {
          auto conn = weak.lock();
          if (!conn) return;
          --conn->pending_packets_;
          if (conn->isDisconnected()) return;
          try {
            conn->outputBuffer_.append(packet.data(), packet.size());
          } catch (...) {
            conn->setState(ConnState::Disconnected);
          }
          if (conn->on_write_ready_) conn->on_write_ready_(conn);
        })) {
      return true;
    }
  } catch (...) {
    // Allocation failed before queue acceptance; the caller may roll back.
  }
  --pending_packets_;
  return false;
}

void Connection::send(const std::string &data) {
  sendPacket(std::vector<char>(data.begin(), data.end()));
}

bool Connection::wantWrite() const {
  return this->outputBuffer_.readableBytes() > 0;
}

Connection::ConnState Connection::state() const { return this->state_.load(); }

void Connection::setState(Connection::ConnState st) {
  if (this->state_.load() != st) {
    std::ostringstream oss;
    oss << "connection state change, fd=" << this->fd_ << ", from=" << static_cast<int>(this->state_.load())
         << ", to=" << static_cast<int>(st);
    LOG_INFO(LogModule::NETWORK, oss.str());
  }
  this->state_.store(st);
}

void Connection::shutdown() {
  if (this->state_ == ConnState::Connected) {
    std::ostringstream oss;
    oss << "connection enter disconnecting, fd=" << this->fd_;
    LOG_INFO(LogModule::NETWORK, oss.str());
    this->state_.store(ConnState::Disconnecting);
  }
}

bool Connection::shouldCloseAfterWrite() const {
  return this->state_ == ConnState::Disconnecting &&
         this->outputBuffer_.readableBytes() == 0;
}

bool Connection::isConnected() const {
  return this->state_ == Connection::ConnState::Connected;
}

bool Connection::isDisconnecting() const {
  return this->state_ == Connection::ConnState::Disconnecting;
}

bool Connection::isDisconnected() const {
  return this->state_ == Connection::ConnState::Disconnected;
}

void Connection::incPendingTasks() { ++this->pending_tasks_; }

void Connection::decPendingTasks() { --this->pending_tasks_; }

bool Connection::hasPendingTasks() const {
  return this->pending_tasks_.load() > 0;
}

bool Connection::canBeClosed() const {
  return this->state_ == ConnState::Disconnecting &&
         this->outputBuffer_.readableBytes() == 0 && !this->hasPendingTasks() &&
         pending_packets_.load() == 0;
}

void Connection::refreshActivity() {
  this->last_active_ms_.store(getSteadyClockMs());
}

uint64_t Connection::lastActiveMs() const {
  return this->last_active_ms_.load();
}
