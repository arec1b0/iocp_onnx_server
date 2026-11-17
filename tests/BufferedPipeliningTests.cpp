#include "server/IocpServer.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

// Drive the same completion and retirement paths as the IOCP workers.
struct IocpServerTestAccess {
    static void EnableProcessing(IocpServer& server) {
        server.stopping_.store(false);
    }

    static IOContext* Complete(IocpServer& server, Connection& connection, IOOperation operation, DWORD bytes) {
        return server.HandleCompletion(connection, operation, bytes);
    }

    static bool DeliverTerminalPacket(IocpServer& server, std::unique_ptr<Connection> owner,
                                      IOContext& pending, bool stopping, bool succeeded, DWORD bytes) {
        auto* connection = owner.get();
        server.connections_.emplace(connection, IocpServer::ActiveConnection{std::move(owner), &pending});
        server.stopping_.store(stopping);
        server.ProcessPacket(connection, &pending.overlapped, succeeded, bytes);
        return server.connections_.empty();
    }
};

namespace {
    void Check(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    std::vector<char> MakeFrame(float value, std::size_t elements = 1) {
        std::vector<char> frame(Protocol::HeaderBytes + elements * Protocol::FloatBytes, 0);
        const std::uint64_t wireCount = elements;
        std::memcpy(frame.data(), &wireCount, Protocol::HeaderBytes);
        std::memcpy(frame.data() + Protocol::HeaderBytes, &value, Protocol::FloatBytes);
        std::memcpy(frame.data() + frame.size() - Protocol::FloatBytes, &value, Protocol::FloatBytes);
        return frame;
    }

    void Append(std::vector<char>& destination, std::span<const char> bytes) {
        destination.insert(destination.end(), bytes.begin(), bytes.end());
    }

    class CompletionDriver {
    public:
        explicit CompletionDriver(std::size_t elements = 1,
                                  IocpServer::Inference inference = [](const std::vector<float>& input) { return input; })
            : server_("127.0.0.1", 0, 1, elements, [this, infer = std::move(inference)](const std::vector<float>& input) {
                  ++inferences;
                  return infer(input);
              }),
              connection_(std::make_unique<Connection>(SocketHandle{}, elements * Protocol::FloatBytes)),
              next_(connection_->PrepareRead()) {
            IocpServerTestAccess::EnableProcessing(server_);
        }

        std::size_t ReadCapacity() const {
            Check(next_ && next_->operation == IOOperation::Read, "expected a receive");
            return next_->wsaBuf.len;
        }

        void Receive(std::span<const char> bytes) {
            Check(bytes.size() <= ReadCapacity(), "test input exceeds posted receive");
            std::copy(bytes.begin(), bytes.end(), next_->wsaBuf.buf);
            ++readCompletions;
            next_ = IocpServerTestAccess::Complete(server_, *connection_, IOOperation::Read,
                                                    static_cast<DWORD>(bytes.size()));
        }

        void Send(std::span<const char> expected, std::size_t bytes) {
            Check(next_ && next_->operation == IOOperation::Write, "expected a response without another receive");
            Check(next_->wsaBuf.len == expected.size() && bytes <= expected.size()
                      && std::ranges::equal(expected, std::span(next_->wsaBuf.buf, next_->wsaBuf.len)),
                  "response bytes are missing, duplicated, or out of order");
            next_ = IocpServerTestAccess::Complete(server_, *connection_, IOOperation::Write,
                                                    static_cast<DWORD>(bytes));
        }

        void FinishSend(std::span<const char> expected) {
            Send(expected, expected.size());
        }

        bool Retired() const { return next_ == nullptr; }

        void Terminate(bool stopping, bool succeeded, DWORD bytes) {
            Check(next_ && next_->operation == IOOperation::Write, "expected a pending write during retirement");
            auto* pending = std::exchange(next_, nullptr);
            Check(IocpServerTestAccess::DeliverTerminalPacket(server_, std::move(connection_),
                                                               *pending, stopping, succeeded, bytes),
                  "terminal packet left a registered connection");
        }

        std::size_t readCompletions = 0;
        std::size_t inferences = 0;

    private:
        IocpServer server_;
        std::unique_ptr<Connection> connection_;
        IOContext* next_;
    };

    void TestCoalesced(std::size_t frameCount, std::size_t elements = 1) {
        CompletionDriver driver(elements);
        const auto readCapacity = driver.ReadCapacity();
        std::vector<std::vector<char>> frames;
        std::vector<char> batch;
        for (std::size_t i = 0; i < frameCount; ++i) {
            frames.push_back(MakeFrame(static_cast<float>(i + 1), elements));
            Append(batch, frames.back());
        }
        driver.Receive(batch);
        for (std::size_t i = 0; i < frameCount; ++i) {
            Check(driver.inferences == i + 1 && driver.readCompletions == 1,
                  "each response must complete before processing the next buffered frame");
            driver.FinishSend(frames[i]);
        }
        Check(driver.ReadCapacity() == readCapacity && driver.inferences == frameCount,
              "coalesced batch left bytes behind or executed an extra request");
    }

    void TestPartialNextFrame(std::size_t split) {
        CompletionDriver driver;
        const auto readCapacity = driver.ReadCapacity();
        const auto first = MakeFrame(1.0f);
        const auto next = MakeFrame(-2.0f);
        const std::span<const char> nextBytes(next);
        auto batch = first;
        Append(batch, nextBytes.first(split));
        driver.Receive(batch);
        driver.FinishSend(first);
        const auto retained = split < Protocol::HeaderBytes ? split : 0;
        Check(driver.ReadCapacity() == readCapacity - retained && driver.inferences == 1,
              "partial next frame did not rearm receive with the expected retained bytes");
        driver.Receive(nextBytes.subspan(split));
        driver.FinishSend(next);
        Check(driver.ReadCapacity() == readCapacity && driver.readCompletions == 2 && driver.inferences == 2,
              "the later receive failed to finish exactly one request");
    }

    void TestBufferedAfterShortWrites() {
        CompletionDriver driver;
        const auto first = MakeFrame(-4.0f);
        const auto second = MakeFrame(3.0f);
        auto batch = first;
        Append(batch, second);
        driver.Receive(batch);
        const std::span<const char> firstBytes(first);
        driver.Send(firstBytes, 3);
        driver.Send(firstBytes.subspan(3), 6);
        Check(driver.inferences == 1, "a short completion started another inference");
        driver.FinishSend(firstBytes.subspan(9));
        Check(driver.readCompletions == 1 && driver.inferences == 2, "final completion failed to drain the buffered frame");
        driver.FinishSend(second);
        Check(driver.ReadCapacity() > 0, "expected a receive after the second response");
    }

    void TestMalformedSecondFrame() {
        CompletionDriver driver;
        const auto first = MakeFrame(5.0f);
        const auto second = MakeFrame(6.0f, 2);
        auto batch = first;
        Append(batch, second);
        driver.Receive(batch);
        driver.FinishSend(first);
        Check(driver.Retired() && driver.inferences == 1 && driver.readCompletions == 1,
              "malformed second frame must retire the connection before inference or receive");
    }

    void TestTerminalPacket(bool stopping, bool succeeded, DWORD bytes) {
        CompletionDriver driver;
        auto batch = MakeFrame(7.0f);
        Append(batch, MakeFrame(8.0f));
        driver.Receive(batch);
        driver.Terminate(stopping, succeeded, bytes);
        Check(driver.inferences == 1, "buffered request ran after disconnect or shutdown");
    }

    void TestDefaultPayloadLimit() {
        constexpr auto elements = Protocol::DefaultMaxPayloadBytes / Protocol::FloatBytes;
        CompletionDriver driver(elements, [](const std::vector<float>& input) {
            Check(input.size() == elements && input.front() == -1.0f && input.back() == -1.0f,
                  "payload at the configured limit did not assemble correctly");
            const std::span<const float> values(input);
            Check(std::ranges::all_of(values.subspan(1, values.size() - 2), [](float value) { return value == 0.0f; }),
                  "payload at the configured limit was corrupted");
            return std::vector<float>{2.0f};
        });
        auto batch = MakeFrame(-1.0f, elements);
        std::array<char, Protocol::HeaderBytes> oversized{};
        const std::uint64_t excessive = elements + 1;
        std::memcpy(oversized.data(), &excessive, oversized.size());
        Append(batch, oversized);
        const auto readCapacity = driver.ReadCapacity();
        std::span<const char> bytes(batch);
        while (bytes.size() > readCapacity) {
            driver.Receive(bytes.first(readCapacity));
            Check(driver.ReadCapacity() == readCapacity && driver.inferences == 0,
                  "large partial payload consumed receive capacity or executed early");
            bytes = bytes.subspan(readCapacity);
        }
        driver.Receive(bytes);
        driver.FinishSend(MakeFrame(2.0f));
        Check(driver.Retired() && driver.inferences == 1, "oversized subsequent prefix at the payload limit was accepted");
    }
}

int main() {
    try {
        TestCoalesced(2);
        TestCoalesced(4);
        // Two frames together fill the 64 KiB receive buffer.
        TestCoalesced(2, (32 * 1024 - Protocol::HeaderBytes) / Protocol::FloatBytes);
        TestPartialNextFrame(3);
        TestPartialNextFrame(Protocol::HeaderBytes + 2);
        TestBufferedAfterShortWrites();
        TestMalformedSecondFrame();
        TestTerminalPacket(false, false, 0);
        TestTerminalPacket(false, true, 0);
        TestTerminalPacket(true, true, Protocol::HeaderBytes + Protocol::FloatBytes);
        TestDefaultPayloadLimit();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
