#pragma once
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
extern char** environ;
#endif

// PARSLEY_CC denotes one executable, never shell syntax or flags.
inline int run_process(const std::vector<std::string>& argv) {
  if (argv.empty()) return -1;
#ifdef _WIN32
  auto wide = [](const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
  };
  std::wstring command;
  for (const auto& arg : argv) {
    if (!command.empty()) command += L' ';
    command += L'"'; size_t slashes = 0;
    for (wchar_t c : wide(arg)) {
      if (c == L'\\') { ++slashes; continue; }
      command.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
      if (c == L'"') command += L'\\'; command += c;
    }
    command.append(slashes * 2, L'\\'); command += L'"';
  }
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return -1;
  WaitForSingleObject(child.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
  CloseHandle(child.hThread); CloseHandle(child.hProcess); return static_cast<int>(code);
#else
  std::vector<char*> args; for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str())); args.push_back(nullptr);
  pid_t pid; if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ)) return -1;
  int status; while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}