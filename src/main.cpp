#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <trantor/net/EventLoopThreadPool.h>
#include <nlohmann/json.hpp>
#include <yaml_cpp_struct.hpp>

struct Config {
	std::string http_config_file;
	std::string chat_html;
	std::string chat_http_path;
	std::string get_http_path;
	std::size_t thread_num;
	std::string openai_url;
	std::unordered_map<std::string, std::string> custom_headers;
	std::optional<std::string> openai_api_key;
	std::string openai_path;
	std::string model;
	double timeout;
	std::size_t http_client_count;
	bool validate_cert;
	bool use_old_tls;
};
YCS_ADD_STRUCT(Config, http_config_file, chat_html, chat_http_path, get_http_path, thread_num,
	openai_url, custom_headers, openai_api_key, openai_path, model, timeout, http_client_count,
	validate_cert, use_old_tls)

auto createResponse(const std::string& error) {
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setContentTypeString("application/json");
	resp->setBody(error);
	return resp;
}

class ChatGpt : public drogon::HttpController<ChatGpt, false> {
public:
	METHOD_LIST_BEGIN
	METHOD_LIST_END

	ChatGpt(const Config& cfg, std::vector<drogon::HttpClientPtr>& http_clients_vec)
		: m_config(cfg)
		, m_http_clients_vec(http_clients_vec) {
		ADD_METHOD_TO(ChatGpt::chat, m_config.chat_http_path, {drogon::Get});
		ADD_METHOD_TO(ChatGpt::get, m_config.get_http_path, {drogon::Post});
	}

	drogon::Task<void> chat(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
	drogon::Task<void> get(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);

private:
	const Config& m_config;
	std::vector<drogon::HttpClientPtr>& m_http_clients_vec;
};

drogon::Task<void> ChatGpt::chat(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)> callback) {
	auto resp = drogon::HttpResponse::newFileResponse(m_config.chat_html);
	callback(resp);
	co_return;
}

drogon::Task<void> ChatGpt::get(drogon::HttpRequestPtr http_request_ptr, std::function<void(const drogon::HttpResponsePtr&)> callback) {
	try {
		nlohmann::json request_content_j = nlohmann::json::parse(http_request_ptr->getBody());
		auto request_content = request_content_j.at("content").get<std::string>();
		SPDLOG_INFO("request_content: {}", request_content);
		drogon::HttpClientPtr http_client_ptr;
		{
			static std::mutex mtx;
			static std::size_t position{0};
			std::lock_guard lk(mtx);
			if (position == m_http_clients_vec.size())
				position = 0;
			http_client_ptr = m_http_clients_vec[position++];
		}
		auto resp = co_await http_client_ptr->sendRequestCoro(
			[&] {
				auto request_ptr = drogon::HttpRequest::newHttpRequest();
				for (auto& [name, value] : m_config.custom_headers)
					request_ptr->addHeader(name, value);
				request_ptr->setContentTypeString("application/json");
				request_ptr->addHeader("Authorization", fmt::format("Bearer {}", m_config.openai_api_key.value()));
				request_ptr->setMethod(drogon::HttpMethod::Post);
				request_ptr->setPath(m_config.openai_path);
				nlohmann::json j;
				j["model"] = m_config.model;
				j["messages"].emplace_back(std::unordered_map<std::string, std::string>{{"role", "user"}, {"content", request_content}});
				SPDLOG_INFO(j.dump());
				request_ptr->setBody(j.dump());
				return request_ptr;
			}(),
			m_config.timeout);
		if (resp->getStatusCode() != drogon::HttpStatusCode::k200OK) {
			SPDLOG_ERROR("openai return error: {}", static_cast<int32_t>(resp->getStatusCode()));
			resp->setPassThrough(true);
			callback(resp);
			co_return;
		}
		nlohmann::json openai_rsp_j = nlohmann::json::parse(resp->getBody());
		SPDLOG_INFO("{}", openai_rsp_j.dump());
		std::string content;
		for (auto choices_vec = openai_rsp_j.at("choices"); auto& info : choices_vec)
			content += info.at("message").at("content").get<std::string>();
		auto chatbot_resp = drogon::HttpResponse::newHttpResponse();
		chatbot_resp->setBody(std::move(content));
		callback(chatbot_resp);
		co_return;
	} catch (const nlohmann::json::exception& e) {
		SPDLOG_ERROR("nlohmann json: {}", e.what());
		callback(createResponse(e.what()));
	} catch (const std::exception& e) {
		SPDLOG_ERROR("{}", e.what());
		callback(createResponse(e.what()));
	}
	co_return;
}

int main(int, char** argv) {
	SPDLOG_INFO("config file: {}", argv[1]);
	auto [config, error] = yaml_cpp_struct::from_yaml<Config>(argv[1]);
	if (!config) {
		SPDLOG_ERROR("{}", error);
		return -1;
	}
	auto& cfg = config.value();
	if (!cfg.openai_api_key) {
		SPDLOG_INFO("load OPENAI_API_KEY from env");
		const char* env = std::getenv("OPENAI_API_KEY");
		if (env == nullptr)
			throw std::runtime_error("can't get OPENAI_API_KEY from env");
		cfg.openai_api_key = std::string{env};
	}
	auto event_thread_pool_ptr = std::make_shared<trantor::EventLoopThreadPool>(cfg.thread_num);
	event_thread_pool_ptr->start();
	std::vector<drogon::HttpClientPtr> http_clients_vec;
	for (std::size_t i = 0; i < cfg.http_client_count; i++) {
		http_clients_vec.emplace_back(drogon::HttpClient::newHttpClient(cfg.openai_url,
			event_thread_pool_ptr->getNextLoop(), cfg.use_old_tls, cfg.validate_cert));
	}
	assert(!http_clients_vec.empty());
	drogon::app()
		.setIntSignalHandler([] { drogon::app().quit(); })
		.registerController(std::make_shared<ChatGpt>(cfg, http_clients_vec))
		.loadConfigFile(cfg.http_config_file)
		.run();
	http_clients_vec.clear();
	event_thread_pool_ptr.reset();
	return 0;
}
