#include <wuwe/agent/process/local_process_backend.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wuwe::agent::process {
namespace {

void append_bounded(
  std::string& output, const char* data, std::size_t size, std::size_t limit, bool& truncated) {
  if (limit == 0) {
    output.append(data, size);
    return;
  }
  if (output.size() >= limit) {
    truncated = truncated || size > 0;
    return;
  }
  const auto count = (std::min)(size, limit - output.size());
  output.append(data, count);
  truncated = truncated || count < size;
}

#ifdef _WIN32

class unique_handle {
public:
  unique_handle() = default;
  explicit unique_handle(HANDLE value) : value_(value) {
  }
  ~unique_handle() {
    reset();
  }
  unique_handle(const unique_handle&) = delete;
  unique_handle& operator=(const unique_handle&) = delete;
  unique_handle(unique_handle&& other) noexcept : value_(other.release()) {
  }
  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept {
    return value_;
  }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE release() noexcept {
    const auto value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value = nullptr) noexcept {
    if (valid())
      CloseHandle(value_);
    value_ = value;
  }

private:
  HANDLE value_ { nullptr };
};

class attribute_list {
public:
  ~attribute_list() {
    if (value_)
      DeleteProcThreadAttributeList(value_);
  }
  bool initialize(HANDLE* handles, std::size_t count) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    storage_.resize(bytes);
    value_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!InitializeProcThreadAttributeList(value_, 1, 0, &bytes)) {
      value_ = nullptr;
      return false;
    }
    return UpdateProcThreadAttribute(value_,
             0,
             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
             handles,
             count * sizeof(HANDLE),
             nullptr,
             nullptr) != FALSE;
  }
  [[nodiscard]] PPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
    return value_;
  }

private:
  std::vector<std::byte> storage_;
  PPROC_THREAD_ATTRIBUTE_LIST value_ { nullptr };
};

std::optional<std::wstring> utf8_to_wide(std::string_view value) {
  if (value.empty())
    return std::wstring {};
  if (value.size() > static_cast<std::size_t>(INT_MAX))
    return std::nullopt;
  const auto size = MultiByteToWideChar(
    CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0)
    return std::nullopt;
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        size) != size)
    return std::nullopt;
  return output;
}

std::wstring quote_argument(std::wstring_view argument) {
  if (argument.empty())
    return L"\"\"";
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring output = L"\"";
  std::size_t slashes = 0;
  for (const auto ch : argument) {
    if (ch == L'\\') {
      ++slashes;
      continue;
    }
    if (ch == L'\"') {
      output.append(slashes * 2 + 1, L'\\');
      output.push_back(L'\"');
      slashes = 0;
      continue;
    }
    output.append(slashes, L'\\');
    slashes = 0;
    output.push_back(ch);
  }
  output.append(slashes * 2, L'\\');
  output.push_back(L'\"');
  return output;
}

struct case_insensitive_less {
  bool operator()(const std::wstring& left, const std::wstring& right) const {
    return _wcsicmp(left.c_str(), right.c_str()) < 0;
  }
};

std::optional<std::vector<wchar_t>> environment_block(
  const process_request& request, std::string& error_message) {
  std::map<std::wstring, std::wstring, case_insensitive_less> values;
  if (request.inherit_parent_environment) {
    auto* block = GetEnvironmentStringsW();
    if (!block) {
      error_message = "failed to read the parent environment";
      return std::nullopt;
    }
    for (auto* current = block; *current; current += std::wcslen(current) + 1) {
      std::wstring entry(current);
      const auto separator = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
      if (separator != std::wstring::npos) {
        values[entry.substr(0, separator)] = entry.substr(separator + 1);
      }
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [key, value] : request.environment) {
    const auto wide_key = utf8_to_wide(key);
    const auto wide_value = utf8_to_wide(value);
    if (!wide_key || !wide_value || wide_key->empty() ||
        wide_key->find(L'=') != std::wstring::npos || wide_key->find(L'\0') != std::wstring::npos ||
        wide_value->find(L'\0') != std::wstring::npos) {
      error_message = "process environment contains invalid UTF-8 or an invalid key";
      return std::nullopt;
    }
    values[*wide_key] = *wide_value;
  }
  std::vector<wchar_t> block;
  for (const auto& [key, value] : values) {
    block.insert(block.end(), key.begin(), key.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (values.empty())
    block.push_back(L'\0');
  constexpr std::size_t maximum_environment_characters = 32767;
  if (block.size() > maximum_environment_characters) {
    error_message = "process environment exceeds the Windows environment-block limit";
    return std::nullopt;
  }
  return block;
}

// 子进程输出经管道传递时，Windows 控制台程序用的是 OEM 代码页（中文系统为 cp936）
// 而不是 UTF-8。原始字节直接存进 std::string 会让下游 JSON 序列化抛
// type_error.316 invalid UTF-8 byte，表现为“命令输出里带中文就随机失败”。
// 整段读完再转码，避免多字节字符被读缓冲切断。
[[nodiscard]] std::string decode_output(const std::string& raw) {
#ifdef _WIN32
  if (raw.empty())
    return raw;
  // 已是合法 UTF-8 就原样保留（POSIX 约定，或子进程自己配了 UTF-8）。
  if (MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0) > 0)
    return raw;
  // ponytail: 只按 OEM 代码页试一次，cmd 与 Python 在中文 Windows 上同为 cp936。
  // 若将来要适配 OEM/ANSI 不一致的语言（如俄语 OEM 866 / ANSI 1251），再回退 ANSI。
  const UINT codepage = GetOEMCP();
  const auto wide_size =
    MultiByteToWideChar(codepage, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
  if (wide_size <= 0)
    return raw;
  std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
  MultiByteToWideChar(
    codepage, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), wide_size);
  const auto utf8_size =
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0)
    return raw;
  std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, utf8.data(), utf8_size, nullptr, nullptr);
  return utf8;
#else
  return raw;
#endif
}

void read_handle(unique_handle owned, std::string& output, std::size_t limit, bool& truncated,
  const std::atomic_bool& stop_io) {
  std::array<char, 8192> buffer {};
  std::string raw;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(owned.get(), nullptr, 0, nullptr, &available, nullptr))
      break;
    if (available == 0) {
      if (stop_io.load(std::memory_order_relaxed))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    DWORD count = 0;
    const auto requested = (std::min)(static_cast<DWORD>(buffer.size()), available);
    if (!ReadFile(owned.get(), buffer.data(), requested, &count, nullptr) || count == 0)
      break;
    append_bounded(raw, buffer.data(), count, limit, truncated);
  }
  output += decode_output(raw);
}

void write_handle(unique_handle owned, const std::string& input) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    DWORD count = 0;
    const auto chunk =
      static_cast<DWORD>((std::min<std::size_t>)(input.size() - offset, 64 * 1024));
    if (!WriteFile(owned.get(), input.data() + offset, chunk, &count, nullptr) || count == 0)
      break;
    offset += count;
  }
}

process_result run_windows(const process_request& request,
  const local_process_backend_config& config, std::stop_token stop_token) {
  process_result result;
  SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
  HANDLE raw_stdin_read = nullptr, raw_stdin_write = nullptr;
  HANDLE raw_stdout_read = nullptr, raw_stdout_write = nullptr;
  HANDLE raw_stderr_read = nullptr, raw_stderr_write = nullptr;
  if (!CreatePipe(&raw_stdin_read, &raw_stdin_write, &security, 0)) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "failed to create process pipes";
    return result;
  }
  unique_handle stdin_read(raw_stdin_read), stdin_write(raw_stdin_write);
  if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &security, 0)) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "failed to create process pipes";
    return result;
  }
  unique_handle stdout_read(raw_stdout_read), stdout_write(raw_stdout_write);
  if (!CreatePipe(&raw_stderr_read, &raw_stderr_write, &security, 0)) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "failed to create process pipes";
    return result;
  }
  unique_handle stderr_read(raw_stderr_read), stderr_write(raw_stderr_write);
  if (!SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "failed to configure process pipe inheritance";
    return result;
  }
  STARTUPINFOEXW startup {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = stdin_read.get();
  startup.StartupInfo.hStdOutput = stdout_write.get();
  startup.StartupInfo.hStdError = stderr_write.get();
  HANDLE inherited[] { stdin_read.get(), stdout_write.get(), stderr_write.get() };
  attribute_list attributes;
  if (!attributes.initialize(inherited, std::size(inherited))) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "failed to restrict inherited process handles";
    return result;
  }
  startup.lpAttributeList = attributes.get();

  auto command_line = quote_argument(request.executable.wstring());
  for (const auto& argument : request.arguments) {
    const auto wide = utf8_to_wide(argument);
    if (!wide) {
      result.termination_reason = process_termination_reason::launch_failed;
      result.error_message = "process argument is not valid UTF-8";
      return result;
    }
    command_line.push_back(L' ');
    command_line += quote_argument(*wide);
  }
  constexpr std::size_t maximum_command_line_characters = 32767;
  if (command_line.size() + 1 > maximum_command_line_characters) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = "process command line exceeds the Windows limit";
    return result;
  }
  std::string environment_error;
  auto environment = environment_block(request, environment_error);
  if (!environment) {
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::move(environment_error);
    return result;
  }

  unique_handle job;
  DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
  if (config.use_process_tree) {
    job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.valid() || !SetInformationJobObject(
                          job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      result.termination_reason = process_termination_reason::launch_failed;
      result.error_message = "failed to configure process job object";
      return result;
    }
    flags |= CREATE_SUSPENDED;
  }

  PROCESS_INFORMATION process {};
  auto executable = request.executable.wstring();
  auto workdir = request.workdir.wstring();
  const auto created = CreateProcessW(executable.c_str(),
    command_line.data(),
    nullptr,
    nullptr,
    TRUE,
    flags,
    environment->data(),
    workdir.empty() ? nullptr : workdir.c_str(),
    &startup.StartupInfo,
    &process);
  stdin_read.reset();
  stdout_write.reset();
  stderr_write.reset();
  if (!created) {
    const auto error = GetLastError();
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::system_category().message(static_cast<int>(error));
    result.metadata["launch_error_code"] = std::to_string(error);
    return result;
  }
  unique_handle process_handle(process.hProcess), thread_handle(process.hThread);
  result.process_id = process.dwProcessId;
  if (job.valid()) {
    if (!AssignProcessToJobObject(job.get(), process_handle.get()) ||
        ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
      TerminateJobObject(job.get(), 1);
      result.termination_reason = process_termination_reason::launch_failed;
      result.error_message = "failed to attach process to its job object";
      return result;
    }
  }

  const auto started = std::chrono::steady_clock::now();
  std::atomic_bool stop_io { false };
  std::thread stdout_thread;
  std::thread stderr_thread;
  std::thread stdin_thread;
  try {
    stdout_thread = std::thread([owned = std::move(stdout_read),
                                  &result,
                                  &stop_io,
                                  limit = request.limits.max_stdout_bytes]() mutable {
      read_handle(std::move(owned), result.stdout_text, limit, result.stdout_truncated, stop_io);
    });
    stderr_thread = std::thread([owned = std::move(stderr_read),
                                  &result,
                                  &stop_io,
                                  limit = request.limits.max_stderr_bytes]() mutable {
      read_handle(std::move(owned), result.stderr_text, limit, result.stderr_truncated, stop_io);
    });
    stdin_thread = std::thread([owned = std::move(stdin_write), &request]() mutable {
      write_handle(std::move(owned), request.stdin_text);
    });
  }
  catch (const std::exception& error) {
    if (job.valid())
      TerminateJobObject(job.get(), 1);
    else
      TerminateProcess(process_handle.get(), 1);
    WaitForSingleObject(process_handle.get(), INFINITE);
    stop_io.store(true, std::memory_order_relaxed);
    if (stdout_thread.joinable())
      stdout_thread.join();
    if (stderr_thread.joinable())
      stderr_thread.join();
    if (stdin_thread.joinable()) {
      CancelSynchronousIo(stdin_thread.native_handle());
      stdin_thread.join();
    }
    result.termination_reason = process_termination_reason::backend_error;
    result.error_message = std::string("failed to start process I/O worker: ") + error.what();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return result;
  }
  bool timed_out = false, cancelled = false, wait_failed = false;
  DWORD wait_error = ERROR_SUCCESS;
  for (;;) {
    const auto wait = WaitForSingleObject(
      process_handle.get(), static_cast<DWORD>(config.cancellation_poll_interval.count()));
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_FAILED) {
      wait_failed = true;
      wait_error = GetLastError();
      if (job.valid())
        TerminateJobObject(job.get(), 1);
      else
        TerminateProcess(process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      break;
    }
    cancelled = stop_token.stop_requested();
    timed_out = !cancelled && request.limits.timeout.count() > 0 &&
                std::chrono::steady_clock::now() - started >= request.limits.timeout;
    if (cancelled || timed_out) {
      if (job.valid())
        TerminateJobObject(job.get(), 1);
      else
        TerminateProcess(process_handle.get(), 1);
      WaitForSingleObject(process_handle.get(), INFINITE);
      break;
    }
  }
  if (job.valid()) {
    // Descendants can keep inherited pipe handles open after the root exits.
    // A bounded command owns its whole job, so settle the tree before joining
    // the pipe-draining threads.
    TerminateJobObject(job.get(), 0);
  }
  stop_io.store(true, std::memory_order_relaxed);
  if (stdin_thread.joinable())
    CancelSynchronousIo(stdin_thread.native_handle());
  stdin_thread.join();
  stdout_thread.join();
  stderr_thread.join();
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  if (wait_failed) {
    result.termination_reason = process_termination_reason::backend_error;
    result.error_message = std::system_category().message(static_cast<int>(wait_error));
    result.metadata["wait_error_code"] = std::to_string(wait_error);
    return result;
  }
  if (cancelled) {
    result.termination_reason = process_termination_reason::cancelled;
    result.error_message = "process cancelled";
    return result;
  }
  if (timed_out) {
    result.termination_reason = process_termination_reason::timed_out;
    result.error_message = "process timed out";
    return result;
  }
  DWORD exit_code = 1;
  if (GetExitCodeProcess(process_handle.get(), &exit_code))
    result.exit_code = static_cast<int>(exit_code);
  result.termination_reason = process_termination_reason::exited;
  return result;
}

#else

class unique_fd {
public:
  unique_fd() = default;
  explicit unique_fd(int value) : value_(value) {
  }
  ~unique_fd() {
    reset();
  }
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  unique_fd(unique_fd&& other) noexcept : value_(other.release()) {
  }
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  [[nodiscard]] int get() const noexcept {
    return value_;
  }
  [[nodiscard]] bool valid() const noexcept {
    return value_ >= 0;
  }
  [[nodiscard]] int release() noexcept {
    const auto value = value_;
    value_ = -1;
    return value;
  }
  void reset(int value = -1) noexcept {
    if (valid())
      ::close(value_);
    value_ = value;
  }

private:
  int value_ { -1 };
};

bool make_nonblocking(int descriptor) {
  const auto flags = ::fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

pid_t waitpid_retry(pid_t pid, int* status, int options) {
  pid_t result = -1;
  do {
    result = ::waitpid(pid, status, options);
  } while (result < 0 && errno == EINTR);
  return result;
}

[[noreturn]] void report_launch_error_and_exit(int descriptor, int launch_error) noexcept {
  std::size_t offset = 0;
  const auto* bytes = reinterpret_cast<const char*>(&launch_error);
  while (offset < sizeof(launch_error)) {
    const auto count = ::write(descriptor, bytes + offset, sizeof(launch_error) - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  _exit(127);
}

void read_fd(unique_fd descriptor, std::string& output, std::size_t limit, bool& truncated,
  const std::atomic_bool& stop_io) {
  std::array<char, 8192> buffer {};
  for (;;) {
    pollfd pending { descriptor.get(), POLLIN, 0 };
    const auto ready = ::poll(&pending, 1, stop_io.load(std::memory_order_relaxed) ? 0 : 25);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      break;
    if (ready == 0) {
      if (stop_io.load(std::memory_order_relaxed))
        break;
      continue;
    }
    if ((pending.revents & (POLLIN | POLLHUP)) != 0) {
      for (;;) {
        const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count > 0) {
          append_bounded(output, buffer.data(), static_cast<std::size_t>(count), limit, truncated);
          continue;
        }
        if (count < 0 && errno == EINTR)
          continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        return;
      }
    }
    if (stop_io.load(std::memory_order_relaxed) || (pending.revents & (POLLERR | POLLNVAL)) != 0)
      break;
  }
}

void write_fd(unique_fd descriptor, const std::string& input, const std::atomic_bool& stop_io) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  if (::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
    return;
  std::size_t offset = 0;
  while (offset < input.size() && !stop_io.load(std::memory_order_relaxed)) {
    pollfd pending { descriptor.get(), POLLOUT, 0 };
    const auto ready = ::poll(&pending, 1, 25);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      break;
    if (ready == 0)
      continue;
    if ((pending.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      break;
    const auto count = ::write(descriptor.get(), input.data() + offset, input.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count <= 0)
      break;
    offset += static_cast<std::size_t>(count);
  }
}

process_result run_posix(const process_request& request, const local_process_backend_config& config,
  std::stop_token stop_token) {
  process_result result;
  int stdin_pipe[2] { -1, -1 }, stdout_pipe[2] { -1, -1 };
  int stderr_pipe[2] { -1, -1 }, launch_pipe[2] { -1, -1 };
  const auto close_pipe = [](int(&pipe_value)[2]) {
    for (auto& descriptor : pipe_value) {
      if (descriptor >= 0)
        ::close(descriptor);
      descriptor = -1;
    }
  };
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0 ||
      ::pipe(launch_pipe) != 0) {
    const auto pipe_error = errno;
    close_pipe(stdin_pipe);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    close_pipe(launch_pipe);
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::strerror(pipe_error);
    return result;
  }
  if (::fcntl(launch_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
    const auto descriptor_error = errno;
    close_pipe(stdin_pipe);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    close_pipe(launch_pipe);
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::strerror(descriptor_error);
    return result;
  }

  std::vector<std::string> argv_storage;
  argv_storage.push_back(request.executable.string());
  argv_storage.insert(argv_storage.end(), request.arguments.begin(), request.arguments.end());
  std::vector<char*> argv;
  for (auto& value : argv_storage)
    argv.push_back(value.data());
  argv.push_back(nullptr);

  std::map<std::string, std::string> environment;
  if (request.inherit_parent_environment && environ) {
    for (auto** current = environ; *current; ++current) {
      std::string entry(*current);
      const auto separator = entry.find('=');
      if (separator != std::string::npos)
        environment[entry.substr(0, separator)] = entry.substr(separator + 1);
    }
  }
  for (const auto& [key, value] : request.environment)
    environment[key] = value;
  std::vector<std::string> env_storage;
  for (const auto& [key, value] : environment)
    env_storage.push_back(key + "=" + value);
  std::vector<char*> envp;
  for (auto& value : env_storage)
    envp.push_back(value.data());
  envp.push_back(nullptr);

  const auto pid = ::fork();
  if (pid < 0) {
    const auto fork_error = errno;
    close_pipe(stdin_pipe);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    close_pipe(launch_pipe);
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::strerror(fork_error);
    return result;
  }
  if (pid == 0) {
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    ::close(launch_pipe[0]);
    const auto set_limit = [&](int resource, std::uint64_t value) {
      if (value == 0)
        return true;
      const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<rlim_t>::max)());
      if (value > maximum) {
        errno = EOVERFLOW;
        return false;
      }
      const rlimit limit { static_cast<rlim_t>(value), static_cast<rlim_t>(value) };
      return ::setrlimit(resource, &limit) == 0;
    };
    const auto cpu_ms = request.limits.max_cpu_time.count();
    const auto cpu_seconds =
      cpu_ms > 0 ? static_cast<std::uint64_t>(cpu_ms / 1000) + (cpu_ms % 1000 == 0 ? 0U : 1U) : 0;
    if ((config.use_process_tree && ::setsid() < 0) || !set_limit(RLIMIT_CPU, cpu_seconds) ||
        ::dup2(stdin_pipe[0], STDIN_FILENO) < 0 || ::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(stderr_pipe[1], STDERR_FILENO) < 0 ||
        (!request.workdir.empty() && ::chdir(request.workdir.c_str()) != 0)) {
      report_launch_error_and_exit(launch_pipe[1], errno);
    }
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    const auto descriptor_limit = ::sysconf(_SC_OPEN_MAX);
    const auto close_until = descriptor_limit > 0 ? descriptor_limit : 1024;
    for (long descriptor = STDERR_FILENO + 1; descriptor < close_until; ++descriptor) {
      if (descriptor != launch_pipe[1])
        ::close(static_cast<int>(descriptor));
    }
    ::execve(request.executable.c_str(), argv.data(), envp.data());
    report_launch_error_and_exit(launch_pipe[1], errno);
  }
  result.process_id = static_cast<std::uint64_t>(pid);
  ::close(stdin_pipe[0]);
  ::close(stdout_pipe[1]);
  ::close(stderr_pipe[1]);
  ::close(launch_pipe[1]);
  int launch_error = 0;
  std::size_t launch_bytes = 0;
  int launch_read_error = 0;
  while (launch_bytes < sizeof(launch_error)) {
    const auto count = ::read(launch_pipe[0],
      reinterpret_cast<char*>(&launch_error) + launch_bytes,
      sizeof(launch_error) - launch_bytes);
    if (count > 0) {
      launch_bytes += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0)
      break;
    if (errno == EINTR)
      continue;
    launch_read_error = errno;
    break;
  }
  ::close(launch_pipe[0]);
  launch_pipe[0] = -1;
  if (launch_read_error != 0 || (launch_bytes != 0 && launch_bytes != sizeof(launch_error))) {
    if (config.use_process_tree)
      ::kill(-pid, SIGKILL);
    else
      ::kill(pid, SIGKILL);
    waitpid_retry(pid, nullptr, 0);
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    result.termination_reason = process_termination_reason::backend_error;
    result.error_message = launch_read_error != 0
                             ? std::strerror(launch_read_error)
                             : "process launch pipe returned a partial error record";
    if (launch_read_error != 0) {
      result.metadata["launch_pipe_error_code"] = std::to_string(launch_read_error);
    }
    return result;
  }
  if (launch_bytes > 0) {
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    waitpid_retry(pid, nullptr, 0);
    result.termination_reason = process_termination_reason::launch_failed;
    result.error_message = std::strerror(launch_error);
    result.metadata["launch_error_code"] = std::to_string(launch_error);
    return result;
  }
  const auto started = std::chrono::steady_clock::now();
  unique_fd stdin_write(stdin_pipe[1]);
  stdin_pipe[1] = -1;
  unique_fd stdout_read(stdout_pipe[0]);
  stdout_pipe[0] = -1;
  unique_fd stderr_read(stderr_pipe[0]);
  stderr_pipe[0] = -1;
  if (!make_nonblocking(stdin_write.get()) || !make_nonblocking(stdout_read.get()) ||
      !make_nonblocking(stderr_read.get())) {
    const auto nonblocking_error = errno;
    if (config.use_process_tree)
      ::kill(-pid, SIGKILL);
    else
      ::kill(pid, SIGKILL);
    waitpid_retry(pid, nullptr, 0);
    result.termination_reason = process_termination_reason::backend_error;
    result.error_message = std::strerror(nonblocking_error);
    return result;
  }
  std::atomic_bool stop_io { false };
  std::thread stdout_thread;
  std::thread stderr_thread;
  std::thread stdin_thread;
  try {
    stdout_thread = std::thread([descriptor = std::move(stdout_read),
                                  &result,
                                  &stop_io,
                                  limit = request.limits.max_stdout_bytes]() mutable {
      read_fd(std::move(descriptor), result.stdout_text, limit, result.stdout_truncated, stop_io);
    });
    stderr_thread = std::thread([descriptor = std::move(stderr_read),
                                  &result,
                                  &stop_io,
                                  limit = request.limits.max_stderr_bytes]() mutable {
      read_fd(std::move(descriptor), result.stderr_text, limit, result.stderr_truncated, stop_io);
    });
    stdin_thread = std::thread([descriptor = std::move(stdin_write), &request, &stop_io]() mutable {
      write_fd(std::move(descriptor), request.stdin_text, stop_io);
    });
  }
  catch (const std::exception& error) {
    if (config.use_process_tree)
      ::kill(-pid, SIGKILL);
    else
      ::kill(pid, SIGKILL);
    waitpid_retry(pid, nullptr, 0);
    stop_io.store(true, std::memory_order_relaxed);
    if (stdin_thread.joinable())
      stdin_thread.join();
    if (stdout_thread.joinable())
      stdout_thread.join();
    if (stderr_thread.joinable())
      stderr_thread.join();
    result.termination_reason = process_termination_reason::backend_error;
    result.error_message = std::string("failed to start process I/O worker: ") + error.what();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    return result;
  }
  bool timed_out = false, cancelled = false;
  int status = 0;
  for (;;) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      break;
    if (waited < 0 && errno == EINTR)
      continue;
    if (waited < 0) {
      const auto wait_error = errno;
      if (config.use_process_tree)
        ::kill(-pid, SIGKILL);
      else
        ::kill(pid, SIGKILL);
      waitpid_retry(pid, &status, 0);
      stop_io.store(true, std::memory_order_relaxed);
      stdin_thread.join();
      stdout_thread.join();
      stderr_thread.join();
      result.termination_reason = process_termination_reason::backend_error;
      result.error_message = std::strerror(wait_error);
      return result;
    }
    cancelled = stop_token.stop_requested();
    timed_out = !cancelled && request.limits.timeout.count() > 0 &&
                std::chrono::steady_clock::now() - started >= request.limits.timeout;
    if (cancelled || timed_out) {
      if (config.use_process_tree)
        ::kill(-pid, SIGKILL);
      else
        ::kill(pid, SIGKILL);
      waitpid_retry(pid, &status, 0);
      break;
    }
    std::this_thread::sleep_for(config.cancellation_poll_interval);
  }
  if (config.use_process_tree) {
    // Ensure descendants cannot outlive the bounded operation or retain its
    // output pipes after the root process exits.
    ::kill(-pid, SIGKILL);
  }
  stop_io.store(true, std::memory_order_relaxed);
  stdin_thread.join();
  stdout_thread.join();
  stderr_thread.join();
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started);
  if (cancelled) {
    result.termination_reason = process_termination_reason::cancelled;
    result.error_message = "process cancelled";
    return result;
  }
  if (timed_out) {
    result.termination_reason = process_termination_reason::timed_out;
    result.error_message = "process timed out";
    return result;
  }
  if (WIFEXITED(status))
    result.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
    result.metadata["signal"] = std::to_string(WTERMSIG(status));
  }
  result.termination_reason = process_termination_reason::exited;
  return result;
}

#endif

} // namespace

local_process_backend::local_process_backend(local_process_backend_config config)
    : config_(config) {
  if (config_.cancellation_poll_interval.count() <= 0) {
    throw std::invalid_argument("cancellation_poll_interval must be positive");
  }
  if (config_.cancellation_poll_interval > std::chrono::seconds(1)) {
    throw std::invalid_argument("cancellation_poll_interval must not exceed one second");
  }
}

sandbox::sandbox_backend_info local_process_backend::info() const {
  const auto cleanup = config_.use_process_tree ? sandbox::enforcement_level::enforced
                                                : sandbox::enforcement_level::not_enforced;
  return {
    .name = "local_process",
    .isolation = sandbox::isolation_level::controlled_process,
    .features = {
      sandbox::sandbox_feature::environment_allowlist,
      sandbox::sandbox_feature::working_directory,
      sandbox::sandbox_feature::stdout_capture,
      sandbox::sandbox_feature::stderr_capture,
      sandbox::sandbox_feature::timeout,
      sandbox::sandbox_feature::cancellation,
    },
    .enforcement = {
      .shell_execution = sandbox::enforcement_level::enforced,
      .timeout = sandbox::enforcement_level::enforced,
      .cancellation = sandbox::enforcement_level::enforced,
      .stdout_limit = sandbox::enforcement_level::enforced,
      .stderr_limit = sandbox::enforcement_level::enforced,
      .environment_allowlist = sandbox::enforcement_level::enforced,
      .working_directory = sandbox::enforcement_level::enforced,
      .process_tree_cleanup = cleanup,
      .process_count_limit = sandbox::enforcement_level::not_enforced,
      .cpu_time_limit = sandbox::enforcement_level::not_enforced,
      .memory_limit = sandbox::enforcement_level::not_enforced,
      .filesystem_read_deny = sandbox::enforcement_level::not_enforced,
      .filesystem_write_deny = sandbox::enforcement_level::not_enforced,
      .network_deny = sandbox::enforcement_level::not_enforced,
    },
  };
}

process_result local_process_backend::run(
  const process_request& request, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return {
      .termination_reason = process_termination_reason::cancelled,
      .error_message = "process cancelled before launch",
    };
  }
#ifdef _WIN32
  return run_windows(request, config_, stop_token);
#else
  return run_posix(request, config_, stop_token);
#endif
}

std::unique_ptr<process_backend> make_local_process_backend(local_process_backend_config config) {
  return std::make_unique<local_process_backend>(config);
}

} // namespace wuwe::agent::process
