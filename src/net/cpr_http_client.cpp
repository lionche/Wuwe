#include <wuwe/net/cpr_http_client.h>

#include <wuwe/net/transport_error.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cpr/cpr.h>

WUWE_NAMESPACE_BEGIN

namespace {

// curl 用 OpenSSL 后端时不读系统证书库；cpr 虽然会设 CURLSSLOPT_NATIVE_CA，但该选项在
// 部分 libcurl 构建里无效（实测 8.13 + OpenSSL 3.6 仍报 "unable to get local issuer
// certificate"），所以这里显式给一个 CA 包。查找顺序：
//   环境变量 DOCGEN_CA_FILE → CURL_CA_BUNDLE → SSL_CERT_FILE → exe 同目录 cacert.pem
// 注意 libcurl 自己并不读这三个环境变量（读它们的是 curl 命令行工具）。
std::string default_ca_bundle() {
  for (const char* name : { "DOCGEN_CA_FILE", "CURL_CA_BUNDLE", "SSL_CERT_FILE" }) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
      return value;
    }
  }
#if defined(_WIN32)
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    std::string candidate(path, static_cast<std::size_t>(length));
    const auto slash = candidate.find_last_of("\\/");
    if (slash != std::string::npos) {
      candidate.resize(slash + 1);
      candidate += "cacert.pem";
      if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return candidate;
      }
    }
  }
#endif
  return {};
}

std::string normalize_http_method(std::string method) {
  std::transform(method.begin(), method.end(), method.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });

  return method;
}

cpr::Header make_cpr_headers(const http_request& request) {
  cpr::Header headers;
  for (const auto& [key, value] : request.headers) {
    headers[key] = value;
  }
  if (!request.trace_id.empty() && headers.find("X-Request-Id") == headers.end()) {
    headers["X-Request-Id"] = request.trace_id;
  }

  return headers;
}

std::vector<http_header> make_response_headers(const cpr::Header& headers) {
  std::vector<http_header> result;
  result.reserve(headers.size());
  for (const auto& [name, value] : headers) {
    result.push_back({ name, value });
  }
  return result;
}

int effective_total_timeout_ms(const http_request& request) {
  return request.timeouts.total_ms > 0 ? request.timeouts.total_ms : request.timeout;
}

void configure_session(cpr::Session& session, const http_request& request) {
  session.SetUrl(cpr::Url { request.url });
  session.SetHeader(make_cpr_headers(request));
  const long max_redirects = request.max_redirects > 0 ? request.max_redirects : 50L;
  session.SetRedirect(cpr::Redirect {
    max_redirects, request.follow_redirects, false, cpr::PostRedirectFlags::POST_ALL });
  session.SetUserAgent(cpr::UserAgent { "Wuwe/1.0" });

  cpr::SslOptions ssl_options = cpr::Ssl(cpr::ssl::NoRevoke { true },
    cpr::ssl::VerifyPeer { request.tls.verify_peer },
    cpr::ssl::VerifyHost { request.tls.verify_host });
  if (!request.tls.ca_file.empty()) {
    ssl_options.SetOption(cpr::ssl::CaInfo { std::filesystem::path { request.tls.ca_file } });
  }
  else if (const auto bundle = default_ca_bundle(); !bundle.empty()) {
    // 没显式指定就退回默认 CA 包，否则 Windows + OpenSSL 下 HTTPS 一律校验失败
    ssl_options.SetOption(cpr::ssl::CaInfo { std::filesystem::path { bundle } });
  }
  if (!request.tls.ca_directory.empty()) {
    ssl_options.SetOption(cpr::ssl::CaPath { request.tls.ca_directory });
  }
  session.SetSslOptions(std::move(ssl_options));

  const int total_timeout = effective_total_timeout_ms(request);
  if (total_timeout > 0) {
    session.SetTimeout(cpr::Timeout { total_timeout });
  }
  if (request.timeouts.connect_ms > 0) {
    session.SetConnectTimeout(cpr::ConnectTimeout { request.timeouts.connect_ms });
  }

  if (!request.body.empty()) {
    session.SetBody(cpr::Body { request.body });
  }

  if (request.proxy && !request.proxy->url.empty()) {
    session.SetProxies(cpr::Proxies {
      { "http", request.proxy->url },
      { "https", request.proxy->url },
    });
    if (!request.proxy->username.empty() || !request.proxy->password.empty()) {
      session.SetProxyAuth(cpr::ProxyAuthentication {
        { "http", cpr::EncodedAuthentication { request.proxy->username, request.proxy->password } },
        { "https",
          cpr::EncodedAuthentication { request.proxy->username, request.proxy->password } },
      });
    }
  }
}

http_response make_http_response(const cpr::Response& response, std::string body = {}) {
  http_response result;
  result.body = body.empty() ? response.text : std::move(body);
  result.status_code = response.status_code;
  result.headers = make_response_headers(response.header);

  if (response.error.code != cpr::ErrorCode::OK) {
    result.transport_error = make_error_code(static_cast<transport_error>(response.error.code));
    result.error_code = result.transport_error;
  }
  else {
    result.error_code = make_error_code(static_cast<http_status_code>(response.status_code));
  }

  return result;
}

} // namespace

http_response cpr_http_client::send(const http_request& request) {
  cpr::Session session;
  configure_session(session, request);

  const std::string method = normalize_http_method(request.method);

  if (method == "GET") {
    return make_http_response(session.Get());
  }
  if (method == "POST") {
    return make_http_response(session.Post());
  }
  if (method == "PUT") {
    return make_http_response(session.Put());
  }
  if (method == "PATCH") {
    return make_http_response(session.Patch());
  }
  if (method == "DELETE") {
    return make_http_response(session.Delete());
  }
  if (method == "HEAD") {
    return make_http_response(session.Head());
  }
  if (method == "OPTIONS") {
    return make_http_response(session.Options());
  }

  return { .error_code = std::make_error_code(std::errc::invalid_argument) };
}

http_response cpr_http_client::send_stream(const http_request& request,
  const http_stream_chunk_callback& on_chunk, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return { .error_code = std::make_error_code(std::errc::operation_canceled) };
  }

  cpr::Session session;
  configure_session(session, request);

  std::string body;
  bool aborted = false;
  session.SetWriteCallback(cpr::WriteCallback([&](std::string_view data, intptr_t) {
    if (stop_token.stop_requested()) {
      aborted = true;
      return false;
    }

    body.append(data);
    if (on_chunk && !on_chunk(data)) {
      aborted = true;
      return false;
    }
    return true;
  }));

  session.SetProgressCallback(cpr::ProgressCallback(
    [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
      if (stop_token.stop_requested()) {
        aborted = true;
        return false;
      }
      return true;
    }));

  const std::string method = normalize_http_method(request.method);
  cpr::Response response;

  if (method == "GET") {
    response = session.Get();
  }
  else if (method == "POST") {
    response = session.Post();
  }
  else if (method == "PUT") {
    response = session.Put();
  }
  else if (method == "PATCH") {
    response = session.Patch();
  }
  else if (method == "DELETE") {
    response = session.Delete();
  }
  else if (method == "HEAD") {
    response = session.Head();
  }
  else if (method == "OPTIONS") {
    response = session.Options();
  }
  else {
    return { .error_code = std::make_error_code(std::errc::invalid_argument) };
  }

  auto result = make_http_response(response, std::move(body));
  if (aborted && !result.error_code) {
    result.transport_error = make_error_code(transport_error::aborted_by_callback);
    result.error_code = result.transport_error;
  }
  return result;
}

WUWE_NAMESPACE_END
