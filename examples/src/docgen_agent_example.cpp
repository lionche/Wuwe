// 代码孪生智能体（CLI）：为目标代码生成《需求规格说明》《软件设计说明》双文档。
// 用法: docgen_agent_example <代码文件或项目目录>
// 自检: docgen_agent_example --check   （不调用 LLM，验证 skill 加载与工具装配）
// skill 目录默认内置，可用环境变量 DOCGEN_SKILL_DIR 覆盖。
#include "console_utf8.hpp"
#include "docgen_agent.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <wuwe/common/print.h>

namespace {

std::string env_or(const char* key, std::string fallback) {
  if (const char* value = std::getenv(key); value && *value) {
    return value;
  }
  return fallback;
}

} // namespace

int main(int argc, char** argv) {
  wuwe_example::configure_utf8_console();
  if (argc != 2) {
    wuwe::println("用法: {} <代码文件或项目目录>  或  --check", argv[0]);
    return 2;
  }

  const auto skill_dir = docgen::skill_dir_for(
    std::filesystem::absolute(argv[0]).parent_path());

  if (std::string_view(argv[1]) == "--check") {
    const auto result = docgen::check(skill_dir);
    if (!result.error.empty()) {
      wuwe::println("{}", result.error);
      return 1;
    }
    wuwe::println("{}", result.summary);
    return 0;
  }

  const docgen::llm_endpoint endpoint {
    .base_url = env_or("DOCGEN_BASE_URL", docgen::default_base_url),
    .api_key = wuwe::llm_config::load_env_value("MIMO_API_KEY"),
    .model = env_or("DOCGEN_MODEL", docgen::default_model),
  };
  // 流式块状态：T=思考 M=正文 \0=无（工具行会打断当前块）
  auto block = std::make_shared<char>('\0');
  const docgen::callbacks cb {
    .on_delta = [block](std::string_view delta) {
      if (*block != 'M') {
        std::cout << '\n';
        *block = 'M';
      }
      std::cout << delta << std::flush;
    },
    .on_reasoning_delta = [block](std::string_view delta) {
      if (*block != 'T') {
        std::cout << "\n[思考] ";
        *block = 'T';
      }
      std::cout << delta << std::flush;
    },
    .on_tool_started = [block](const std::string& name, const std::string& arguments_json) {
      *block = '\0';
      wuwe::println("\n⏳ [工具] {} {}", name, arguments_json.substr(0, 120));
    },
    .on_tool = [block](const std::string& name, const std::string&, const std::string&) {
      *block = '\0';
      wuwe::println("✓ [工具] {}", name);
    },
  };

  const auto result =
    docgen::run(skill_dir, std::filesystem::absolute(argv[1]), endpoint, {}, cb);
  if (!result.error.empty()) {
    wuwe::println("\n错误: {}", result.error);
    return 1;
  }
  wuwe::println("\n完成。文档目录: {}", result.out_dir.string());
  return 0;
}
