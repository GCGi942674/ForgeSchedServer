#include "TestSupport.h"
#include "Connection.h"
#include <fcntl.h>
#include <cstring>
struct Pair {
    Fd peer;
    std::shared_ptr<Connection> connection;
    Pair() {
        int fds[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        peer.value = fds[0];
        CHECK(::fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
        connection = std::make_shared<Connection>(nullptr, fds[1]);
    }
};
int main() { return testMain([] {
    {
        Pair p;
        std::string data(8192, 'x');
        uint32_t length = htonl(MessageCodec::kMaxBodyLenght + 1);
        std::memcpy(&data[0], &length, 4);
        sendBytes(p.peer.value, data.data(), data.size());
        auto r = p.connection->handleRead();
        CHECK(r.decode_error && !r.ok);
        CHECK(r.bytes_received == 4); // Reject at the header, not after draining the socket.
    }
    {
        Pair p;
        const auto packet = MessageCodec::encode("x");
        std::vector<char> stream;
        for (int n = 0; n < 150; ++n) stream.insert(stream.end(), packet.begin(), packet.end());
        sendBytes(p.peer.value, stream.data(), stream.size());
        int seen = 0;
        p.connection->setMessageCallback([&](const auto& c, const std::string& msg) {
            CHECK(msg == "x"); ++seen; c->decPendingTasks();
        });
        auto first = p.connection->handleRead();
        CHECK(first.messages_decoded == Connection::kReadBudgetMessages);
        while (seen < 150) {
            auto r = p.connection->handleRead();
            CHECK(r.ok && r.messages_decoded <= Connection::kReadBudgetMessages);
        }
        CHECK(!p.connection->hasPendingTasks());
    }
    {
        Pair p;
        const std::string body(MessageCodec::kMaxBodyLenght, 'z');
        const auto packet = MessageCodec::encode(body);
        bool received = false;
        p.connection->setMessageCallback([&](const auto& c, const std::string& msg) {
            CHECK(msg == body); received = true; c->decPendingTasks();
        });
        // Feed bounded chunks; this also exercises split headers and maximum-size frames.
        size_t offset = 0;
        while (offset < packet.size()) {
            const auto count = std::min<size_t>(32768, packet.size() - offset);
            sendBytes(p.peer.value, packet.data() + offset, count);
            offset += count;
            auto r = p.connection->handleRead();
            CHECK(r.ok && r.bytes_received <= Connection::kReadBudgetBytes);
        }
        CHECK(received);
        CHECK(::shutdown(p.peer.value, SHUT_WR) == 0);
        CHECK(p.connection->handleRead().peer_close);
    }
    {
        Pair p;
        const int send_capacity = 256 * 1024;
        CHECK(::setsockopt(p.peer.value, SOL_SOCKET, SO_SNDBUF, &send_capacity, sizeof(send_capacity)) == 0);
        // More than one turn is already readable; byte budget must still stop recv.
        std::string prefix(128 * 1024, 'z');
        uint32_t length = htonl(MessageCodec::kMaxBodyLenght);
        std::memcpy(&prefix[0], &length, 4);
        sendBytes(p.peer.value, prefix.data(), prefix.size());
        auto r = p.connection->handleRead();
        CHECK(r.ok && r.bytes_received == Connection::kReadBudgetBytes);
        CHECK(r.messages_decoded == 0);
        r = p.connection->handleRead();
        CHECK(r.ok && r.bytes_received == Connection::kReadBudgetBytes);
    }
    {
        Pair p;
        const auto packet = MessageCodec::encode("fragmented");
        sendBytes(p.peer.value, packet.data(), 2);
        CHECK(p.connection->handleRead().messages_decoded == 0);
        sendBytes(p.peer.value, packet.data()+2, packet.size()-2);
        CHECK(p.connection->handleRead().messages_decoded == 1);
    }
}); }
