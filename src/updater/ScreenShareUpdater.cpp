#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wofstream& logStream()
{
    static std::wofstream stream;
    if (!stream.is_open()) {
        wchar_t tempPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempPath);
        fs::path logPath = fs::path(tempPath) / L"ScreenShare" / L"updates" / L"ScreenShareUpdater.log";
        std::error_code error;
        fs::create_directories(logPath.parent_path(), error);
        stream.open(logPath, std::ios::app);
    }
    return stream;
}

void log(std::wstring_view message)
{
    auto& stream = logStream();
    if (stream.is_open()) {
        stream << message << L"\n";
    }
}

void logError(std::wstring_view message, const std::error_code& error)
{
    auto& stream = logStream();
    if (stream.is_open()) {
        stream << message << L": " << error.message().c_str() << L"\n";
    }
}

std::vector<std::wstring> commandLineArguments()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> result;
    if (argv != nullptr) {
        result.reserve(static_cast<size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            result.emplace_back(argv[index]);
        }
        LocalFree(argv);
    }
    return result;
}

std::wstring argumentValue(const std::vector<std::wstring>& arguments, std::wstring_view name)
{
    for (size_t index = 1; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return {};
}

std::wstring powershellLiteral(const fs::path& path)
{
    std::wstring value = path.wstring();
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'\'');
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            escaped.append(L"''");
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back(L'\'');
    return escaped;
}

bool runProcess(const std::wstring& applicationPath, std::wstring commandLine, DWORD timeoutMs)
{
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    if (!CreateProcessW(
            applicationPath.empty() ? nullptr : applicationPath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {
        log(L"CreateProcessW failed");
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (waitResult != WAIT_OBJECT_0 || exitCode != 0) {
        log(L"Child process failed or timed out");
        return false;
    }
    return true;
}

bool waitForAppExit(DWORD processId)
{
    if (processId == 0) {
        return true;
    }

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return true;
    }

    const DWORD waitResult = WaitForSingleObject(process, 60000);
    CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0) {
        log(L"Timed out waiting for ScreenShareUi to exit");
        return false;
    }
    return true;
}

bool extractPackage(const fs::path& packagePath, const fs::path& extractDir)
{
    std::error_code error;
    fs::remove_all(extractDir, error);
    fs::create_directories(extractDir, error);
    if (error) {
        logError(L"Could not create extraction directory", error);
        return false;
    }

    // Resolve the interpreter by its full System32 path and pass it as the
    // application name. With a NULL application name and a relative
    // "powershell.exe", CreateProcessW searches the current/loaded-from
    // directory first, so a planted powershell.exe in the temp updates
    // directory (attacker-writable by the same user) could hijack the update.
    wchar_t systemDir[MAX_PATH] = {};
    const UINT systemDirLength = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (systemDirLength == 0 || systemDirLength >= MAX_PATH) {
        log(L"Could not resolve the system directory for powershell.exe");
        return false;
    }
    const std::wstring powerShellPath =
        std::wstring(systemDir, systemDirLength) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

    // -Command with an inline expression is not governed by the script
    // execution policy, so -ExecutionPolicy Bypass is unnecessary.
    std::wstring command =
        L"\"" + powerShellPath + L"\" -NoProfile -Command \"Expand-Archive -LiteralPath " +
        powershellLiteral(packagePath) +
        L" -DestinationPath " +
        powershellLiteral(extractDir) +
        L" -Force\"";
    return runProcess(powerShellPath, std::move(command), 120000);
}

bool moveOrCopyDirectory(const fs::path& source, const fs::path& destination)
{
    std::error_code error;
    fs::rename(source, destination, error);
    if (!error) {
        return true;
    }

    error.clear();
    fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    if (error) {
        logError(L"Could not copy package directory into place", error);
        return false;
    }

    fs::remove_all(source, error);
    return true;
}

bool isCMakeBuildDirectory(const fs::path& path)
{
    return fs::exists(path / L"CMakeCache.txt") || fs::exists(path / L"CMakeFiles");
}

fs::path findExtractedPackageRoot(const fs::path& extractDir)
{
    if (fs::exists(extractDir / L"ScreenShareUi.exe")) {
        return extractDir;
    }

    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(extractDir, error)) {
        if (entry.is_directory() && fs::exists(entry.path() / L"ScreenShareUi.exe")) {
            return entry.path();
        }
    }
    return {};
}

void preserveUserFiles(const fs::path& backupDir, const fs::path& targetDir)
{
    std::error_code error;
    for (const wchar_t* dirName : {L"reports", L"logs", L"config"}) {
        const fs::path source = backupDir / dirName;
        if (fs::exists(source)) {
            fs::copy(source, targetDir / dirName, fs::copy_options::recursive | fs::copy_options::skip_existing, error);
            error.clear();
        }
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(backupDir, error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path source = entry.path();
        const std::wstring filename = source.filename().wstring();
        const std::wstring extension = source.extension().wstring();
        const bool reportZip = extension == L".zip" && filename.find(L"report") != std::wstring::npos;
        const bool logFile = extension == L".log";
        if (reportZip || logFile) {
            fs::copy_file(source, targetDir / source.filename(), fs::copy_options::skip_existing, error);
            error.clear();
        }
    }
}

bool replaceInstallFolder(const fs::path& packageRoot, const fs::path& targetDir)
{
    if (isCMakeBuildDirectory(targetDir)) {
        log(L"Refusing to update a CMake build directory");
        return false;
    }

    const fs::path backupDir = targetDir.parent_path() /
        (targetDir.filename().wstring() + L".update-backup");

    std::error_code error;
    fs::remove_all(backupDir, error);
    error.clear();
    fs::rename(targetDir, backupDir, error);
    if (error) {
        logError(L"Could not move current install folder to backup", error);
        return false;
    }

    if (!moveOrCopyDirectory(packageRoot, targetDir)) {
        log(L"Could not move new package into place; restoring backup");
        std::error_code restoreError;
        fs::remove_all(targetDir, restoreError);
        restoreError.clear();
        fs::rename(backupDir, targetDir, restoreError);
        return false;
    }

    preserveUserFiles(backupDir, targetDir);
    fs::remove_all(backupDir, error);
    return true;
}

void restartApp(const fs::path& restartExe, const fs::path& workingDir)
{
    ShellExecuteW(
        nullptr,
        L"open",
        restartExe.c_str(),
        nullptr,
        workingDir.c_str(),
        SW_SHOWNORMAL);
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const std::vector<std::wstring> arguments = commandLineArguments();
    const std::wstring pidText = argumentValue(arguments, L"--pid");
    const fs::path packagePath = argumentValue(arguments, L"--package");
    const fs::path targetDir = argumentValue(arguments, L"--target");
    const fs::path restartExe = argumentValue(arguments, L"--restart");

    if (packagePath.empty() || targetDir.empty() || restartExe.empty()) {
        log(L"Missing required updater arguments");
        return 2;
    }

    DWORD processId = 0;
    if (!pidText.empty()) {
        processId = static_cast<DWORD>(std::wcstoul(pidText.c_str(), nullptr, 10));
    }

    if (!waitForAppExit(processId)) {
        return 3;
    }

    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    const fs::path extractDir = fs::path(tempPath) / L"ScreenShare" / L"updates" / L"extracted";
    if (!extractPackage(packagePath, extractDir)) {
        return 4;
    }

    const fs::path packageRoot = findExtractedPackageRoot(extractDir);
    if (packageRoot.empty()) {
        log(L"Extracted package did not contain ScreenShareUi.exe");
        return 5;
    }

    if (!replaceInstallFolder(packageRoot, targetDir)) {
        return 6;
    }

    restartApp(restartExe, targetDir);
    return 0;
}
