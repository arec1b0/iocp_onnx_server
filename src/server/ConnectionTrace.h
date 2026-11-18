#pragma once
#include <winsock2.h>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <vector>
#include "ConnectionMetrics.h"

// Diagnostic-only storage. The server serializes producers with its registry lock.
// Read the buffer only after Stop() has joined the accept and worker threads.
class ConnectionTrace {
public:
    struct Peer {
        std::uint64_t id = 0;
        sockaddr_in local{};
        sockaddr_in remote{};
        std::int64_t acceptedAtUs = 0;
    };

    Peer Identify(SOCKET socket, const sockaddr_in* remote) noexcept {
        Peer peer{++nextId_};
        peer.acceptedAtUs = UtcMicroseconds();
        int length = sizeof(peer.local);
        getsockname(socket, reinterpret_cast<sockaddr*>(&peer.local), &length);
        if (remote) peer.remote = *remote;
        return peer;
    }

    void Record(Peer peer, const char* event, const char* phase, const char* operation, unsigned long code,
                unsigned long bytes, std::uint64_t active, std::uint64_t pending, std::uint64_t capacity) noexcept {
        records_[written_ % records_.size()] = {peer, UtcMicroseconds(), event, phase, operation,
                                                code, bytes, active, pending, capacity};
        ++written_;
    }

    bool Save() const {
        std::ofstream file("connection-trace.jsonl", std::ios::trunc);
        if (!file) return false;
        const auto first = written_ > records_.size() ? written_ - records_.size() : 0;
        file << "{\"event\":\"trace_metadata\",\"recorded\":" << written_ << ",\"overwritten\":" << first << "}\n";
        for (auto sequence = first; sequence < written_; ++sequence) {
            const auto& item = records_[sequence % records_.size()];
            file << "{\"event\":\"" << item.event << "\",\"phase\":\"" << item.phase << "\",\"operation\":\""
                 << item.operation << "\",\"seq\":" << sequence + 1 << ",\"timestamp_us\":" << item.timestampUs
                 << ",\"connection_id\":" << item.peer.id << ",\"accepted_at_us\":" << item.peer.acceptedAtUs;
            WriteEndpoint(file, "local", item.peer.local);
            WriteEndpoint(file, "remote", item.peer.remote);
            file << ",\"code\":" << item.code << ",\"bytes\":" << item.bytes
                 << ",\"active\":" << item.active << ",\"pending\":" << item.pending
                 << ",\"capacity\":" << item.capacity << "}\n";
        }
        file.close();
        return !file.fail();
    }

private:
    struct Event {
        Peer peer;
        std::int64_t timestampUs;
        const char* event;
        const char* phase;
        const char* operation;
        unsigned long code;
        unsigned long bytes;
        std::uint64_t active;
        std::uint64_t pending;
        std::uint64_t capacity;
    };

    static std::int64_t UtcMicroseconds() noexcept {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    }

    static void WriteEndpoint(std::ostream& file, const char* key, const sockaddr_in& address) {
        const auto ip = ntohl(address.sin_addr.s_addr);
        file << ",\"" << key << "\":\"" << ((ip >> 24) & 255) << '.' << ((ip >> 16) & 255) << '.'
             << ((ip >> 8) & 255) << '.' << (ip & 255) << ':' << ntohs(address.sin_port) << '"';
    }

    std::vector<Event> records_ = std::vector<Event>(32768);
    std::uint64_t written_ = 0;
    std::uint64_t nextId_ = 0;
};
