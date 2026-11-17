#include "inference/TensorUtils.h"
#include "server/Connection.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
    using Protocol::ReadResult;

    // Three float32 values (1, -2.5, 3) with an explicit little-endian prefix.
    constexpr std::array<char, 20> Frame{
        3, 0, 0, 0, 0, 0, 0, 0,
        0, 0, '\x80', '\x3f', 0, 0, '\x20', '\xc0', 0, 0, '\x40', '\x40'
    };
    constexpr auto PayloadBytes = Frame.size() - Protocol::HeaderBytes;

    void Check(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    ReadResult SimulateRead(Connection& conn, std::span<const char> chunk, std::vector<float>& input) {
        auto* ctx = conn.PrepareRead();
        Check(chunk.size() <= ctx->wsaBuf.len, "test chunk exceeds posted receive length");
        std::copy(chunk.begin(), chunk.end(), ctx->wsaBuf.buf);
        return conn.ReadRequest(chunk.size(), input);
    }

    void TestFragmentation() {
        const std::vector<float> expected{1.0f, -2.5f, 3.0f};
        const std::span<const char> frame(Frame);
        for (std::size_t split = 1; split < frame.size(); ++split) {
            Connection conn(SocketHandle{}, PayloadBytes);
            std::vector<float> input;
            Check(SimulateRead(conn, frame.first(split), input) == ReadResult::Incomplete,
                  "a partial frame was not held");
            Check(input.capacity() == 0, "input tensor allocated before a complete frame");
            Check(SimulateRead(conn, frame.subspan(split), input) == ReadResult::Complete,
                  "a split frame was not completed");
            Check(input == expected, "split frame values differ from the wire payload");
        }

        Connection conn(SocketHandle{}, PayloadBytes);
        std::vector<float> input;
        for (std::size_t i = 0; i < frame.size(); ++i) {
            const auto expectedResult = i + 1 == frame.size() ? ReadResult::Complete : ReadResult::Incomplete;
            Check(SimulateRead(conn, frame.subspan(i, 1), input) == expectedResult,
                  "byte-by-byte frame status differs");
        }
        Check(input == expected, "byte-by-byte frame values differ");

        const auto readCapacity = conn.PrepareRead()->wsaBuf.len;
        std::array<char, Frame.size() + 5> coalesced{};
        std::copy(Frame.begin(), Frame.end(), coalesced.begin());
        std::copy_n(Frame.begin(), 5, coalesced.begin() + Frame.size());
        Check(SimulateRead(conn, coalesced, input) == ReadResult::Complete, "coalesced first frame lost");
        Check(conn.PrepareRead()->wsaBuf.len == readCapacity - 5, "receive would overwrite retained prefix");
        Check(SimulateRead(conn, frame.subspan(5), input) == ReadResult::Complete, "retained frame lost");
        Check(input == expected, "retained frame values differ");
    }

    void TestLargePayload() {
        constexpr std::size_t elements = 32769;
        constexpr auto payloadBytes = elements * Protocol::FloatBytes;
        std::vector<char> frame(Protocol::HeaderBytes + payloadBytes, 0);
        frame[0] = 1;
        frame[1] = '\x80';

        Connection conn(SocketHandle{}, payloadBytes);
        const auto readCapacity = conn.PrepareRead()->wsaBuf.len;
        const std::span<const char> bytes(frame);
        std::vector<float> input;
        Check(SimulateRead(conn, bytes.first(readCapacity), input) == ReadResult::Incomplete,
              "first full receive completed a large frame early");
        Check(SimulateRead(conn, bytes.subspan(readCapacity, readCapacity), input) == ReadResult::Incomplete,
              "second full receive completed a large frame early");
        Check(SimulateRead(conn, bytes.subspan(2 * readCapacity), input) == ReadResult::Complete,
              "final receive did not complete a large frame");
        Check(input.size() == elements && std::ranges::all_of(input, [](float value) { return value == 0.0f; }),
              "large frame values differ");
    }

    void TestInvalidPrefixes() {
        constexpr std::array<std::array<char, Protocol::HeaderBytes>, 5> invalid{{
            {0}, {2}, {4}, {0, 0, 0, 0, 0, 0, 0, '\x40'},
            {'\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff'}
        }};
        for (const auto& header : invalid) {
            for (std::size_t split = 1; split < header.size(); ++split) {
                Connection conn(SocketHandle{}, PayloadBytes);
                std::vector<float> input;
                const std::span<const char> bytes(header);
                Check(SimulateRead(conn, bytes.first(split), input) == ReadResult::Incomplete,
                      "short prefix was parsed early");
                Check(SimulateRead(conn, bytes.subspan(split), input) == ReadResult::Invalid,
                      "invalid count was not rejected on the final prefix byte");
                Check(input.capacity() == 0, "invalid count allocated a tensor");
            }

            Connection conn(SocketHandle{}, PayloadBytes);
            std::vector<float> input;
            std::vector<char> receive(conn.PrepareRead()->wsaBuf.len, 0);
            std::copy(header.begin(), header.end(), receive.begin());
            Check(SimulateRead(conn, receive, input) == ReadResult::Invalid, "invalid prefix with extra data accepted");
            Check(input.capacity() == 0, "invalid prefix with extra data allocated a tensor");
        }
    }

    void TestSizeBoundaries() {
        using TensorUtils::GetTensorElementsCount;
        constexpr auto maxElements = Protocol::MaxPayloadBytes / Protocol::FloatBytes;
        constexpr auto maxDimension = (std::numeric_limits<std::int64_t>::max)();
        Check(GetTensorElementsCount({1, 10}, 10) == 10, "exact configured limit rejected");
        Check(!GetTensorElementsCount({1, 10}, 9), "configured limit not enforced");
        Check(GetTensorElementsCount({maxElements}, maxElements) == maxElements, "maximum wire payload rejected");
        Check(!GetTensorElementsCount({maxElements + 1}, maxElements), "oversized wire payload accepted");
        Check(!GetTensorElementsCount({2, maxDimension}, maxElements), "oversized shape product accepted");
        Check(!GetTensorElementsCount({maxDimension, 3}, (std::numeric_limits<std::size_t>::max)()),
              "shape product outside size_t accepted");
        Check(GetTensorElementsCount({maxDimension, 3, 0}, maxElements) == 0, "zero dimension lost");
        Check(GetTensorElementsCount({}, maxElements) == 0, "empty shape behavior changed");

        Connection conn(SocketHandle{}, 0);
        std::vector<float> input;
        constexpr std::array<char, Protocol::HeaderBytes> emptyFrame{};
        Check(SimulateRead(conn, emptyFrame, input) == ReadResult::Complete && input.empty(),
              "zero-element request for a zero-element model was not retained");
    }
}

int main() {
    try {
        TestFragmentation();
        TestLargePayload();
        TestInvalidPrefixes();
        TestSizeBoundaries();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
