#include "server/Connection.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
    constexpr std::array FirstOutput{1.0f, -2.5f, 3.0f};
    constexpr std::array SecondOutput{4.0f};
    constexpr std::array<char, 20> FirstFrame{
        3, 0, 0, 0, 0, 0, 0, 0,
        0, 0, '\x80', '\x3f', 0, 0, '\x20', '\xc0', 0, 0, '\x40', '\x40'
    };
    constexpr std::array<char, 12> SecondFrame{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\x80', '\x40'};

    void Check(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void CheckSend(const IOContext* context, std::span<const char> expected) {
        Check(context != nullptr, "expected a send");
        Check(context->wsaBuf.len == expected.size(), "send length differs from the expected suffix");
        Check(std::equal(expected.begin(), expected.end(), context->wsaBuf.buf),
              "send bytes differ from the expected suffix");
    }

    void TestShortCompletions() {
        Connection conn(SocketHandle{}, 0);
        IOContext* const context = conn.PrepareResponse(FirstOutput);
        CheckSend(context, FirstFrame);
        const char* const start = context->wsaBuf.buf;
        std::vector<char> transmitted;
        std::size_t offset = 0;

        // The first two completions stop inside the header. The third stops inside the payload.
        const std::array<std::size_t, 4> completions{3, 4, 6, 7};
        for (const auto bytes : completions) {
            Check(conn.PrepareResponse(SecondOutput) == nullptr, "a new frame overwrote an outstanding send");
            Check(context->wsaBuf.buf == start + offset, "send buffer moved or restarted");
            CheckSend(context, std::span(FirstFrame).subspan(offset));
            transmitted.insert(transmitted.end(), context->wsaBuf.buf, context->wsaBuf.buf + bytes);
            offset += bytes;

            context->overlapped.InternalHigh = bytes;
            const auto completion = conn.CompleteSend(true, bytes);
            Check(completion.succeeded, "a valid completion failed");
            if (offset < FirstFrame.size()) {
                Check(completion.remaining == context, "short completion did not rearm the write context");
                Check(context->overlapped.InternalHigh == 0, "reposted OVERLAPPED was not reset");
            } else {
                Check(completion.remaining == nullptr, "final completion scheduled another send");
            }
        }

        IOContext* const next = conn.PrepareResponse(SecondOutput);
        CheckSend(next, SecondFrame);
        transmitted.insert(transmitted.end(), next->wsaBuf.buf, next->wsaBuf.buf + next->wsaBuf.len);
        const auto exact = conn.CompleteSend(true, next->wsaBuf.len);
        Check(exact.succeeded && exact.remaining == nullptr, "exact completion did not finish the next response");

        std::vector<char> expected(FirstFrame.begin(), FirstFrame.end());
        expected.insert(expected.end(), SecondFrame.begin(), SecondFrame.end());
        Check(transmitted == expected, "sequential responses were corrupted or out of order");

        constexpr std::array<char, Protocol::HeaderBytes> emptyFrame{};
        const auto* empty = conn.PrepareResponse({});
        CheckSend(empty, emptyFrame);
        const auto emptyCompletion = conn.CompleteSend(true, empty->wsaBuf.len);
        Check(emptyCompletion.succeeded && emptyCompletion.remaining == nullptr,
              "header-only response did not complete");
    }

    void TestFailedCompletion(bool succeeded, std::size_t bytes) {
        Connection conn(SocketHandle{}, 0);
        CheckSend(conn.PrepareResponse(FirstOutput), FirstFrame);
        const auto partial = conn.CompleteSend(true, 3);
        Check(partial.succeeded, "first completion failed");
        CheckSend(partial.remaining, std::span(FirstFrame).subspan(3));

        partial.remaining->overlapped.InternalHigh = 3;
        const auto failure = conn.CompleteSend(succeeded, bytes);
        Check(!failure.succeeded && failure.remaining == nullptr, "failed completion allowed another operation");
        Check(partial.remaining->overlapped.InternalHigh == 3, "failed completion reset the write context");
        Check(conn.PrepareResponse(SecondOutput) == nullptr, "send failure allowed the buffer to be reused");
        CheckSend(partial.remaining, std::span(FirstFrame).subspan(3));
    }
}

int main() {
    try {
        TestShortCompletions();
        TestFailedCompletion(false, 0);
        TestFailedCompletion(false, 7);
        TestFailedCompletion(true, 0);
        TestFailedCompletion(true, FirstFrame.size() - 2);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
