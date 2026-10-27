#pragma once
#include "baker.h"
#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <fstream>
#ifdef _WIN32
// clang-format off
#define NOMINMAX
#include <windows.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace arkit::test {
struct ProcessResult {
  int status;
  std::string output;
};
inline ProcessResult execute(const std::vector<std::string> &args,
                             const fs::path &directory, bool isolated = false) {
  auto output = directory / "process-output.txt";
#ifdef _WIN32
  auto wide = [](const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (!n)
      throw std::runtime_error("Invalid UTF-8 test argument");
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), w.data(), n);
    return w;
  };
  // Quote argv according to the Windows C runtime rules, without a shell.
  std::wstring line;
  for (const auto &arg : args) {
    if (!line.empty())
      line += L' ';
    line += L'"';
    std::size_t slashes = 0;
    for (wchar_t c : wide(arg)) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      line.append(slashes * (c == L'"' ? 2 : 1), L'\\');
      slashes = 0;
      if (c == L'"')
        line += L'\\';
      line += c;
    }
    line.append(slashes * 2, L'\\');
    line += L'"';
  }
  std::vector<wchar_t> environment;
  if (isolated) {
    std::vector<std::wstring> entries;
    auto block = GetEnvironmentStringsW();
    if (!block)
      throw std::runtime_error("Cannot read environment");
    for (auto p = block; *p; p += wcslen(p) + 1) {
      std::wstring entry = p;
      auto name = entry.substr(0, entry.find(L'='));
      if (_wcsicmp(name.c_str(), L"PATH") == 0 ||
          _wcsnicmp(name.c_str(), L"PYTHON", 6) == 0)
        continue;
      entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(block);
    entries.push_back(L"PATH=");
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
      return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    for (const auto &entry : entries) {
      environment.insert(environment.end(), entry.begin(), entry.end());
      environment.push_back(0);
    }
    environment.push_back(0);
  }
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE file =
      CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot capture subprocess output");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  auto program = wide(args.front());
  BOOL created = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr,
                                TRUE, CREATE_UNICODE_ENVIRONMENT,
                                isolated ? environment.data() : nullptr,
                                directory.c_str(), &startup, &process);
  CloseHandle(file);
  if (!created)
    throw std::runtime_error("Cannot start test subprocess: " +
                             std::to_string(GetLastError()));
  auto waited = WaitForSingleObject(process.hProcess, 180000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    throw std::runtime_error("Test subprocess timed out");
  }
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  pid_t pid = fork();
  if (pid < 0)
    throw std::runtime_error("Cannot fork test subprocess");
  if (pid == 0) {
    int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || chdir(directory.c_str()))
      _exit(127);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    if (isolated) {
      setenv("PATH", "", 1);
      for (auto key :
           {"PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"})
        unsetenv(key);
    }
    std::vector<char *> argv;
    for (const auto &a : args)
      argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  int code = 0;
  while (waitpid(pid, &code, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("Cannot wait for test subprocess");
  }
  int status = WIFEXITED(code) ? WEXITSTATUS(code) : 128 + WTERMSIG(code);
#endif
  std::ifstream stream(output, std::ios::binary);
  return {static_cast<int>(status),
          std::string(std::istreambuf_iterator<char>(stream), {})};
}
} // namespace arkit::test
