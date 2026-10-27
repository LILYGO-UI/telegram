#include "tdjson_auth_fixture.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace {

std::mutex mutex;
std::condition_variable ready;
std::deque<std::string> responses;
std::map<int, std::string> auth_states;
std::set<std::string> request_ids;
std::map<std::string, std::pair<std::string, std::string>> response_overrides;
std::map<std::string, int> request_counts;
int created_clients = 0;
int parameter_requests = 0;
int logout_requests = 0;
int phone_requests = 0;
int duplicate_request_ids = 0;

// The fixture accepts the compact JSON emitted by this application's adapter.
std::string field_value(const std::string& payload, const std::string& name)
{
    auto offset = payload.find('"' + name + '"');
    if (offset == std::string::npos) return {};
    offset = payload.find(':', offset);
    if (offset == std::string::npos) return {};
    offset = payload.find_first_not_of(" \t", offset + 1);
    if (offset == std::string::npos) return {};
    const auto end = payload[offset] == '"' ? payload.find('"', offset + 1) + 1
                                            : payload.find_first_of(",}", offset);
    return payload.substr(offset, end - offset);
}

std::string auth_state(const std::string& type)
{
    std::string result = "{\"@type\":\"" + type + "\"";
    if (type == "authorizationStateWaitOtherDeviceConfirmation")
        result += ",\"link\":\"tg://login?token=test-only\"";
    return result + "}";
}

void enqueue(int client_id, std::string payload, const std::string& extra = {})
{
    payload.pop_back();
    payload += ",\"@client_id\":" + std::to_string(client_id);
    if (!extra.empty()) payload += ",\"@extra\":" + extra;
    responses.push_back(payload + "}");
    ready.notify_all();
}

void update_auth(int client_id, const std::string& state)
{
    auth_states[client_id] = state;
    enqueue(client_id, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":" + auth_state(state) + "}");
}

}  // namespace

extern "C" int td_create_client_id()
{
    std::lock_guard<std::mutex> lock(mutex);
    const int id = ++created_clients;
    update_auth(id, "authorizationStateWaitTdlibParameters");
    return id;
}

extern "C" void td_send(int client_id, const char* request)
{
    std::lock_guard<std::mutex> lock(mutex);
    const std::string payload = request;
    const auto type = field_value(payload, "@type");
    const auto extra = field_value(payload, "@extra");
    if (!extra.empty() && !request_ids.insert(extra).second) ++duplicate_request_ids;
    ++request_counts[type];
    const auto configured = response_overrides.find(type);
    if (configured != response_overrides.end()) {
        enqueue(client_id, configured->second.first, extra);
        if (!configured->second.second.empty()) enqueue(client_id, configured->second.second);
        return;
    }

    if (type == "\"getAuthorizationState\"") {
        enqueue(client_id, auth_state(auth_states.at(client_id)), extra);
    } else if (type == "\"getOption\"") {
        enqueue(client_id, R"({"@type":"optionValueString","value":"test"})", extra);
    } else if (type == "\"setTdlibParameters\"") {
        ++parameter_requests;
        enqueue(client_id, R"({"@type":"ok"})", extra);
        update_auth(client_id, "authorizationStateWaitPhoneNumber");
    } else if (type == "\"requestQrCodeAuthentication\"") {
        enqueue(client_id, R"({"@type":"ok"})", extra);
        update_auth(client_id, "authorizationStateWaitOtherDeviceConfirmation");
    } else if (type == "\"logOut\"") {
        ++logout_requests;
        enqueue(client_id, R"({"@type":"ok"})", extra);
        update_auth(client_id, "authorizationStateLoggingOut");
        update_auth(client_id, "authorizationStateClosing");
        update_auth(client_id, "authorizationStateClosed");
        // Responses from the old client can remain in TDLib's global queue.
        enqueue(client_id, R"({"@type":"updateAuthorizationState","authorization_state":{"@type":"authorizationStateWaitOtherDeviceConfirmation","link":"tg://login?token=stale-client"}})");
    } else if (type == "\"close\"") {
        update_auth(client_id, "authorizationStateClosed");
    } else if (type == "\"setAuthenticationPhoneNumber\"") {
        ++phone_requests;
        if (auth_states.at(client_id) == "authorizationStateWaitPhoneNumber") {
            enqueue(client_id, R"({"@type":"ok"})", extra);
            update_auth(client_id, "authorizationStateWaitCode");
        } else {
            enqueue(client_id, R"({"@type":"error","code":400,"message":"Phone number submitted in the wrong state"})", extra);
        }
    } else {
        enqueue(client_id, R"({"@type":"ok"})", extra);
    }
}

extern "C" const char* td_receive(double timeout)
{
    static thread_local std::string current;
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait_for(lock, std::chrono::duration<double>(timeout), [] { return !responses.empty(); });
    if (responses.empty()) return nullptr;
    current = std::move(responses.front());
    responses.pop_front();
    return current.c_str();
}

extern "C" void telegram_auth_fixture_reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    responses.clear();
    auth_states.clear();
    request_ids.clear();
    response_overrides.clear();
    request_counts.clear();
    created_clients = parameter_requests = logout_requests = phone_requests = duplicate_request_ids = 0;
}

extern "C" void telegram_auth_fixture_set_server_ready()
{
    std::lock_guard<std::mutex> lock(mutex);
    // Keep this update off the queue: the reset probe must discover the race.
    auth_states.at(created_clients) = "authorizationStateReady";
}

extern "C" void telegram_auth_fixture_authorize_current_client()
{
    std::lock_guard<std::mutex> lock(mutex);
    update_auth(created_clients, "authorizationStateReady");
}

extern "C" void telegram_auth_fixture_set_response(const char* request_type, const char* payload,
                                                    const char* following_update)
{
    std::lock_guard<std::mutex> lock(mutex);
    response_overrides.insert_or_assign('"' + std::string(request_type) + '"',
        std::make_pair(std::string(payload), following_update ? std::string(following_update) : std::string{}));
}

extern "C" void telegram_auth_fixture_emit_updates(const char* const* updates, int count)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < count; ++i) enqueue(created_clients, updates[i]);
}

extern "C" int telegram_auth_fixture_request_count(const char* request_type)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = request_counts.find('"' + std::string(request_type) + '"');
    return found == request_counts.end() ? 0 : found->second;
}

extern "C" int telegram_auth_fixture_created_clients()
{
    std::lock_guard<std::mutex> lock(mutex);
    return created_clients;
}

extern "C" int telegram_auth_fixture_parameter_requests()
{
    std::lock_guard<std::mutex> lock(mutex);
    return parameter_requests;
}

extern "C" int telegram_auth_fixture_logout_requests()
{
    std::lock_guard<std::mutex> lock(mutex);
    return logout_requests;
}

extern "C" int telegram_auth_fixture_phone_requests()
{
    std::lock_guard<std::mutex> lock(mutex);
    return phone_requests;
}

extern "C" int telegram_auth_fixture_duplicate_request_ids()
{
    std::lock_guard<std::mutex> lock(mutex);
    return duplicate_request_ids;
}
