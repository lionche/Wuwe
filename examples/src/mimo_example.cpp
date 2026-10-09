#include "console_utf8.hpp"

#include <wuwe/wuwe.h>

int main() {
  wuwe_example::configure_utf8_console();
  wuwe::llm_config config {
    .api_key = wuwe::llm_config::load_env_value("MIMO_API_KEY"),
    .base_url = "https://token-plan-cn.xiaomimimo.com/v1",
    .model = "mimo-v2.6-flash",
  };

  auto client = wuwe::llm_client_factory{}.create_shared("OpenAI", config);
  const auto response = client->complete("用一句话介绍你自己。");

  wuwe::println("{}", response.content);
}
