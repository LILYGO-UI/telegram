#ifndef TELEGRAM_TEST_TDJSON_AUTH_FIXTURE_HPP
#define TELEGRAM_TEST_TDJSON_AUTH_FIXTURE_HPP

extern "C" {
void telegram_auth_fixture_reset();
void telegram_auth_fixture_set_server_ready();
void telegram_auth_fixture_authorize_current_client();
void telegram_auth_fixture_set_response(const char* request_type, const char* payload, const char* following_update);
void telegram_auth_fixture_emit_updates(const char* const* updates, int count);
int telegram_auth_fixture_request_count(const char* request_type);
int telegram_auth_fixture_created_clients();
int telegram_auth_fixture_parameter_requests();
int telegram_auth_fixture_logout_requests();
int telegram_auth_fixture_phone_requests();
int telegram_auth_fixture_duplicate_request_ids();
}

#endif
