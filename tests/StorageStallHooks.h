#pragma once

// This header is included only by the isolated diagnostics build.
#include <Windows.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace StorageProbe {
    inline std::wstring Setting(const wchar_t* name) {
        wchar_t value[256]{};
        const auto length = GetEnvironmentVariableW(name, value, 256);
        if (length >= 256) std::abort();
        return std::wstring(value, length);
    }

    inline void EnterGate() {
        static const auto prefix = Setting(L"IOCP_STAGE25_EVENT");
        if (prefix.empty()) std::abort();
        const auto entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L"-entered").c_str());
        const auto release = OpenEventW(SYNCHRONIZE, FALSE, (prefix + L"-release").c_str());
        if (!entered || !release || !SetEvent(entered)) std::abort();
        const auto status = WaitForSingleObject(release, INFINITE);
        CloseHandle(release);
        CloseHandle(entered);
        if (status != WAIT_OBJECT_0) std::abort();
    }

    inline std::ostream& Append(std::ofstream& stream, const char* record, std::streamsize size) {
        static const auto operation = Setting(L"IOCP_STAGE25_SINK");
        if (operation == L"append") EnterGate();
        if (operation == L"partial" || operation == L"permanent") {
            if (operation == L"partial") {
                if (size < 7) std::abort();
                stream.write(record, 7);
            }
            stream.setstate(std::ios::badbit);
            return stream;
        }
        return stream.write(record, size);
    }

    inline std::ostream& Flush(std::ofstream& stream) {
        if (Setting(L"IOCP_STAGE25_SINK") == L"flush") EnterGate();
        return stream.flush();
    }

    inline void Close(std::ofstream& stream) {
        if (Setting(L"IOCP_STAGE25_SINK") == L"close") EnterGate();
        stream.close();
    }
}
