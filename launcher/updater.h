#pragma once

#include <bcrypt.h>
#include <cstdio>
#include <string>

class LauncherUpdater {
public:
    enum class State { Idle, Checking, Current, Available, Downloading, Ready, Error };

    void check()
    {
        discard();
        state = start(L"Check", L"", true) ? State::Checking : State::Error;
    }

    void download()
    {
        unsigned char random[16];
        if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            state = State::Error;
            return;
        }
        stage.clear();
        for (unsigned char byte : random) {
            constexpr char hex[] = "0123456789abcdef";
            stage += hex[byte >> 4];
            stage += hex[byte & 15];
        }
        state = start(L"Stage", arguments(), true) ? State::Downloading : State::Error;
    }

    bool apply()
    {
        if (!start(L"Apply", arguments() + L" -Digest " + wide(digest) +
                   L" -LauncherPid " + std::to_wstring(GetCurrentProcessId()), false)) {
            state = State::Error;
            return false;
        }
        stage.clear();
        return true;
    }

    void poll()
    {
        if (!process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            return;
        DWORD code = 1, bytes = 0;
        char buffer[128];
        std::string output;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr) && bytes && bytes < sizeof buffer &&
            ReadFile(pipe, buffer, bytes, &bytes, nullptr)) {
            output.assign(buffer, bytes);
            while (!output.empty() && (output.back() == '\r' || output.back() == '\n'))
                output.pop_back();
        }
        GetExitCodeProcess(process, &code);
        close_process();

        if (state == State::Checking && code == 10) {
            state = State::Current;
        } else if (state == State::Checking && code == 0 && valid_version(output)) {
            version = output;
            state = State::Available;
        } else if (state == State::Downloading && code == 0 && output.size() == 64 &&
                   output.find_first_not_of("0123456789abcdef") == std::string::npos) {
            digest = output;
            state = State::Ready;
        } else {
            discard();
            state = State::Error;
        }
    }

    void finish()
    {
        if (process) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 500);
            close_process();
        }
        discard();
    }

    State state = State::Idle;
    std::string version;

    static std::string installed_version()
    {
        const std::wstring root = installation_directory();
        if (!root.empty()) {
            if (FILE *marker = _wfopen((root + L"ShiningSource-version.txt").c_str(), L"rb")) {
                char line[32] = {};
                const bool read = fgets(line, sizeof line, marker) != nullptr;
                fclose(marker);
                if (read) {
                    std::string number(line);
                    number.erase(number.find_last_not_of("\r\n") + 1);
                    if (valid_version(number))
                        return number;
                }
            }
        }
        return "0.9.5";
    }

private:
    HANDLE process = nullptr, pipe = nullptr;
    std::string stage, digest;

    static std::wstring wide(const std::string &ascii)
    {
        return std::wstring(ascii.begin(), ascii.end());
    }

    static bool valid_version(const std::string &number)
    {
        return number.size() >= 3 && number.size() <= 29 &&
               number.find_first_not_of("0123456789.") == std::string::npos;
    }

    static std::wstring installation_directory()
    {
        wchar_t executable[32768];
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768)
            return {};
        std::wstring root(executable, length);
        root.resize(root.find_last_of(L"\\/") + 1);
        return root;
    }

    std::wstring arguments() const
    {
        return L" -Version " + wide(version) + L" -StageId " + wide(stage);
    }

    void close_process()
    {
        CloseHandle(process);
        CloseHandle(pipe);
        process = pipe = nullptr;
    }

    void discard()
    {
        if (!stage.empty()) {
            start(L"Discard", L" -StageId " + wide(stage), false);
            stage.clear();
        }
    }

    bool start(const wchar_t *action, const std::wstring &args, bool capture)
    {
        wchar_t system[MAX_PATH];
        const std::wstring root = installation_directory();
        const UINT system_length = GetSystemDirectoryW(system, MAX_PATH);
        if (root.empty() || !system_length || system_length >= MAX_PATH)
            return false;
        const std::wstring script = root + L"ShiningSource-update.ps1";
        if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES)
            return false;
        const std::wstring powershell = std::wstring(system, system_length) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring command = L"\"" + powershell + L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                               script + L"\" -Action " + action + args;
        HANDLE input = nullptr, output = nullptr, null_input = INVALID_HANDLE_VALUE;
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        if (capture) {
            SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
            if (!CreatePipe(&input, &output, &security, 0))
                return false;
            if (!SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0)) {
                CloseHandle(input);
                CloseHandle(output);
                return false;
            }
            null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (null_input == INVALID_HANDLE_VALUE) {
                CloseHandle(input);
                CloseHandle(output);
                return false;
            }
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = null_input;
            startup.hStdOutput = output;
            startup.hStdError = output;
        }
        PROCESS_INFORMATION child{};
        const BOOL started = CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, capture,
                                            CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &child);
        if (capture) {
            CloseHandle(output);
            CloseHandle(null_input);
        }
        if (!started) {
            if (capture)
                CloseHandle(input);
            return false;
        }
        CloseHandle(child.hThread);
        if (capture) {
            process = child.hProcess;
            pipe = input;
        } else {
            CloseHandle(child.hProcess);
        }
        return true;
    }
};
