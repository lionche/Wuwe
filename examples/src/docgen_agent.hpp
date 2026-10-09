// 代码文档智能体共用装配：skill 加载激活、文件/进程工具装配、agent runner 组装。
// CLI（docgen_agent_example）与 GUI（docgen-gui）共用，逻辑改动只应发生在这里。
#ifndef DOCGEN_AGENT_HPP
#define DOCGEN_AGENT_HPP

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wuwe/agent/core/content.hpp>
#include <wuwe/agent/filesystem/filesystem_tools.hpp>
#include <wuwe/agent/filesystem/local_filesystem_backend.hpp>
#include <wuwe/agent/llm/llm_agent_runner.h>
#include <wuwe/agent/llm/llm_provider_factory.h>
#include <wuwe/agent/process/local_process_backend.hpp>
#include <wuwe/agent/process/process_tools.hpp>
#include <wuwe/agent/skills/skills.hpp>

namespace docgen {

inline constexpr const char* default_skill_dir =
  "/Users/che/Documents/GitHub/DocTwin/skills/cpp-code-to-docs";
inline constexpr const char* default_base_url = "https://opencode.ai/zen/go/v1";
inline constexpr const char* default_model = "deepseek-v4.1-flash";

struct llm_endpoint {
  std::string base_url;
  std::string api_key;
  std::string model;
};

struct callbacks {
  std::function<void(std::string_view)> on_delta;
  std::function<void(std::string_view)> on_reasoning_delta;
  std::function<void(const std::string& name, const std::string& arguments_json)>
    on_tool_started;
  std::function<void(const std::string& name, const std::string& arguments_json,
    const std::string& result)>
    on_tool;
  std::function<void(const std::string&)> on_info;
};

struct check_result {
  std::string error;
  std::string summary;
};

struct run_result {
  std::string error;
  std::filesystem::path out_dir;
};

namespace detail {

namespace fs = wuwe::agent::filesystem;
namespace proc = wuwe::agent::process;
namespace skills = wuwe::agent::skills;

// 合并文件与进程两组工具，按工具名分发到对应 provider。
struct tool_provider {
  std::shared_ptr<fs::filesystem_tool_provider> files;
  std::shared_ptr<proc::process_tool_provider> processes;

  [[nodiscard]] std::vector<wuwe::llm_tool> tools() const {
    auto list = files->tools();
    auto extra = processes->tools();
    list.insert(list.end(), extra.begin(), extra.end());
    return list;
  }

  [[nodiscard]] wuwe::llm_tool_result invoke(
    const std::string& name, const std::string& arguments_json) const {
    for (const auto& tool : files->tools()) {
      if (tool.name == name) {
        return files->invoke(name, arguments_json);
      }
    }
    return processes->invoke(name, arguments_json);
  }
};

struct agent_parts {
  skills::skill_activation_result activation;
  std::string skill_summary;
  std::filesystem::path python;
  std::unique_ptr<fs::filesystem_runtime> files_runtime;
  std::unique_ptr<proc::process_runtime> processes_runtime;
  std::shared_ptr<tool_provider> tools;
};

inline std::string prepare(const std::filesystem::path& skill_dir,
  const std::filesystem::path& workdir, agent_parts& out) {
  skills::directory_skill_loader loader({
    .root = skill_dir.parent_path(),
    .trust = wuwe::agent::core::content_trust_level::application_trusted,
  });
  const auto loaded = loader.load(skill_dir.filename());
  if (!loaded) {
    return "skill 加载失败 [" + std::string(skills::to_string(loaded.error)) + "]: " +
           loaded.message;
  }
  skills::skill_registry registry;
  if (!registry.register_package(loaded.package)) {
    return "skill 注册失败";
  }
  const auto& descriptor = loaded.package->descriptor();
  const auto resolved = skills::skill_resolver().resolve(registry.snapshot(),
    { .roots = { { .id = descriptor.id,
                   .version = skills::version_requirement::parse(
                     "^" + descriptor.version.string()) } } });
  if (!resolved) {
    return "skill 解析失败";
  }
  out.activation = skills::skill_activator().activate({ .resolution = resolved });
  if (!out.activation) {
    return "skill 激活失败";
  }
  out.skill_summary = "skill 已激活: " + descriptor.id + "@" + descriptor.version.string() +
                      "（指令 " + std::to_string(out.activation.instructions.size()) + " 份）";

  out.files_runtime = std::make_unique<fs::filesystem_runtime>(fs::make_local_filesystem_backend(),
    fs::filesystem_policy {
      .root = "/",
      .allow_absolute_paths = true,
      .allow_write = true,
      .allow_create_directory = true,
      .require_approval_for_write = false,
      .require_approval_for_move = false,
      .require_approval_for_remove = false,
    });

  // Python 解释器：捆绑运行时（Windows）> skill 项目 venv > PATH 名称。
  std::error_code error;
  const auto project_root = skill_dir.parent_path().parent_path();
  std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
  candidates.push_back(project_root / "runtime" / "python" / "python.exe");
  candidates.emplace_back("python");
#else
  candidates.push_back(project_root / ".venv" / "bin" / "python3");
  candidates.emplace_back("python3");
#endif
  out.python = candidates.back();
  for (const auto& candidate : candidates) {
    if (!candidate.has_parent_path() || std::filesystem::exists(candidate, error)) {
      out.python = candidate;
      break;
    }
  }

  std::vector<std::filesystem::path> executables;
  std::vector<std::filesystem::path> search_paths;
#ifdef _WIN32
  executables.emplace_back("python");
  executables.emplace_back("python.exe");
  const char* system_root = std::getenv("SystemRoot");
  search_paths.push_back(
    std::filesystem::path(system_root ? system_root : "C:\\Windows") / "System32");
  const char* comspec = std::getenv("COMSPEC");
  const std::string shell_executable = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
#else
  executables = { "python3", "rm" };
  for (const auto* dir : { "/usr/bin", "/opt/homebrew/bin", "/bin" }) {
    if (std::filesystem::is_directory(dir, error)) {
      search_paths.push_back(dir);
    }
  }
  const std::string shell_executable = "/bin/sh";
#endif
  if (out.python.has_parent_path()) {
    executables.push_back(out.python);
  }
  out.processes_runtime =
    std::make_unique<proc::process_runtime>(proc::make_local_process_backend(),
      proc::process_policy {
        .working_directory_root = workdir.root_path(),
        .default_workdir = workdir,
        .allowed_executables = executables,
        .executable_search_paths = search_paths,
        .allow_shell = true,
        .require_approval_for_process = false,
        .require_approval_for_shell = false,
        .shell_executable = shell_executable,
      });

  out.tools = std::make_shared<tool_provider>(tool_provider {
    std::make_shared<fs::filesystem_tool_provider>(*out.files_runtime),
    std::make_shared<proc::process_tool_provider>(*out.processes_runtime,
      proc::process_tool_options { .expose_shell_tool = true }),
  });
  return {};
}

} // namespace detail

// 加载 skill 并装配工具，不调用 LLM。workdir 用于进程工具的默认工作目录。
[[nodiscard]] inline check_result check(const std::filesystem::path& skill_dir,
  const std::filesystem::path& workdir = std::filesystem::current_path()) {
  detail::agent_parts parts;
  if (auto error = detail::prepare(skill_dir, workdir, parts); !error.empty()) {
    return { .error = std::move(error) };
  }
  std::string summary = parts.skill_summary + "\n工具装配 OK，共 " +
                        std::to_string(parts.tools->tools().size()) + " 个:";
  for (const auto& tool : parts.tools->tools()) {
    summary += "\n  - " + tool.name;
  }
  return { .summary = std::move(summary) };
}

// 端到端执行：按 skill 流程驱动 LLM 与工具，生成双文档到 <目标目录>/output/docs。
[[nodiscard]] inline run_result run(const std::filesystem::path& skill_dir,
  const std::filesystem::path& target, const llm_endpoint& endpoint,
  std::stop_token stop_token = {}, const callbacks& cb = {}) {
  std::error_code error;
  if (!std::filesystem::exists(target, error)) {
    return { .error = "目标不存在: " + target.string() };
  }
  if (endpoint.api_key.empty()) {
    return { .error = "缺少 API key（设置 MIMO_API_KEY 或在界面填写）" };
  }
  const auto project_dir =
    std::filesystem::is_directory(target, error) ? target : target.parent_path();
  const auto out_dir = project_dir / "output" / "docs";

  detail::agent_parts parts;
  if (auto prepare_error = detail::prepare(skill_dir, project_dir, parts);
      !prepare_error.empty()) {
    return { .error = std::move(prepare_error) };
  }
  if (cb.on_info) {
    cb.on_info(parts.skill_summary);
  }

  wuwe::llm_config config {
    .base_url = endpoint.base_url,
    .api_key = endpoint.api_key,
    .model = endpoint.model,
    .timeout = 600000, // 生成整份文档 + 多轮工具调用，30s 不够
  };
  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenAI", config);

  const std::string prompt = "请按已加载的 cpp-code-to-docs skill 流程，为目标 " +
                             target.string() + " 生成《需求规格说明.docx》和《软件设计说明.docx》，" +
                             "输出到 " + out_dir.string() + "。\n" + "skill 安装目录: " +
                             skill_dir.string() + "（scripts/、references/ 需要时用工具读取或运行，" +
                             "Python 解释器使用 " + parts.python.string() + "）\n" +
                             "skill 流程第 8 步的 Documents skill（render_docx.py）不可用时跳过渲染验收，"
                             "直接交付两份 docx。";

  wuwe::llm_request request;
  request.model = config.model;
  request.messages.push_back({ .role = "user", .content = prompt });
  request = detail::skills::apply_skill_activation(std::move(request), parts.activation,
    { .trusted_instructions_as_system_messages = true, .expose_activated_tools = false });

  wuwe::llm_agent_runner runner(*client, parts.tools, 64);
  wuwe::llm_agent_run_options options;
  options.stop_token = stop_token;
  if (cb.on_delta) {
    options.callbacks.on_delta = [&cb](std::string_view delta) { cb.on_delta(delta); };
  }
  if (cb.on_reasoning_delta) {
    options.callbacks.on_reasoning_delta = [&cb](std::string_view delta) {
      cb.on_reasoning_delta(delta);
    };
  }
  if (cb.on_tool_started) {
    options.callbacks.on_tool_start = [&cb](const wuwe::llm_tool_call& call) {
      cb.on_tool_started(call.name, call.arguments_json);
    };
  }
  if (cb.on_tool) {
    options.callbacks.on_tool_result = [&cb](
                                         const wuwe::llm_tool_call& call,
                                         const wuwe::llm_tool_result& result) {
      cb.on_tool(call.name, call.arguments_json, result.content);
    };
  }
  // 预算用尽时让模型直接收尾，而不是硬失败退出。
  options.callbacks.finalize_after_tool_round_budget =
    [](const wuwe::llm_request&, const wuwe::llm_response&, std::size_t used,
       std::size_t max) -> std::optional<wuwe::llm_agent_tool_budget_finalization> {
    return wuwe::llm_agent_tool_budget_finalization {
      .instruction = "工具轮次预算已用完（" + std::to_string(used) + "/" +
                     std::to_string(max) +
                     "）。不要再调用工具，直接输出最终总结：已生成的文档路径、完成情况、待确认事项。",
    };
  };

  const auto response = runner.complete(std::move(request), std::move(options));
  if (response.error_code) {
    // content 里是服务端错误正文；不带出来就只剩 "API error" 这种查不出原因的字符串
    const auto detail = response.content.substr(0, 600);
    return { .error = response.error_code.message() +
                       (detail.empty() ? std::string {} : ": " + detail),
             .out_dir = out_dir };
  }
  return { .out_dir = out_dir };
}

} // namespace docgen

#endif // DOCGEN_AGENT_HPP
