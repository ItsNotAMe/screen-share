#include "CaptureTestWindow.h"
#include "capture/DesktopCapturer.h"
#include <iostream>

namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
class ChildSource {
public:
    ChildSource() {
        job_.value = CreateJobObjectW(nullptr, nullptr);
        Require(job_.value != nullptr, "Source job creation failed");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Source job limits failed");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        Require(CreatePipe(&read, &write, &attributes, 0), "Create source pipe failed");
        struct Pipe { HANDLE a, b; ~Pipe() { CloseHandle(a); CloseHandle(b); } } pipe{read, write};
        Require(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0), "Source pipe inheritance failed");
        wchar_t executable[32768];
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        Require(length && length < 32768, "Source executable path failed");
        std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --child";
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError = write;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        Require(CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &process_), "Source process creation failed");
        try {
            Require(AssignProcessToJobObject(job_.value, process_.hProcess), "Source job assignment failed");
            Require(ResumeThread(process_.hThread) != DWORD(-1), "Source process resume failed");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            DWORD bytes = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                Require(PeekNamedPipe(read, nullptr, 0, nullptr, &bytes, nullptr), "Source pipe unavailable");
                if (bytes >= sizeof(window_)) break;
                Require(WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT, "Source process exited before readiness");
                Sleep(5);
            }
            Require(bytes >= sizeof(window_), "Source window startup timeout");
            Require(ReadFile(read, &window_, sizeof(window_), &bytes, nullptr) && bytes == sizeof(window_) && IsWindow(window_),
                "Invalid source window handshake");
        } catch (...) { Close(); throw; }
    }
    ~ChildSource() { Close(); }
    HWND window() const { return window_; }
    void Close() {
        if (process_.hProcess) {
            TerminateProcess(process_.hProcess, 0);
            WaitForSingleObject(process_.hProcess, 5000);
            CloseHandle(process_.hProcess); CloseHandle(process_.hThread);
            process_ = {};
        }
    }
private:
    struct Job { HANDLE value = nullptr; ~Job() { if (value) CloseHandle(value); } } job_;
    PROCESS_INFORMATION process_{};
    HWND window_ = nullptr;
};
void Run() {
    ChildSource source;
    screenshare::DesktopCapturer capture;
    screenshare::CaptureConfig config;
    config.sourceType = screenshare::CaptureSourceType::Window;
    config.windowHandle = reinterpret_cast<uint64_t>(source.window());
    config.targetWidth = 640; config.targetHeight = 360;
    config.includeNv12 = config.ownedNv12 = true;
    config.includeNv12Readback = config.includeBgraReadback = false;
    capture.Start(config);
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!received && std::chrono::steady_clock::now() < deadline)
        received = bool(capture.TryCaptureFrame(std::chrono::milliseconds(20)));
    Require(received, "External source capture timed out");
    source.Close();
    bool closed = false;
    try { capture.TryCaptureFrame(std::chrono::milliseconds(20)); } catch (const std::runtime_error&) { closed = true; }
    Require(closed && capture.sourceState() == screenshare::CaptureSourceState::Closed, "External process exit was not detected");
    capture.Stop(); capture.Stop();
    Require(capture.sourceState() == screenshare::CaptureSourceState::Stopped, "Capture did not stop");
    std::cout << "External source process exit detected; no replacement source; clean capture shutdown.\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--child") {
            proof::TestWindow window;
            HWND handle = window.handle(); DWORD written;
            Require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), &handle, sizeof(handle), &written, nullptr) && written == sizeof(handle), "Source handshake failed");
            Sleep(INFINITE); // Parent owns this proof process and terminates it.
            return 0;
        }
        for (int cycle = 0; cycle < 3; ++cycle) Run();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
