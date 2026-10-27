#ifndef LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_TDLIB_CLIENT_TEST_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_TDLIB_CLIENT_TEST_HPP

#include "domain/telegram_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace telegram::tdlib_test {

struct AuthMapping {
    AuthState state = AuthState::Unknown;
    std::string prompt;
};

AuthMapping map_authorization_state(std::string_view json);
ConnectionState map_connection_state(std::string_view json);
FileInfo map_file(std::string_view json);
Chat map_chat(std::string_view json);
Message map_message(std::string_view json);
Message map_message_with_senders(std::string_view json, const std::vector<std::string>& users,
                                 const std::vector<std::string>& chats);
bool apply_sender_update(Message& message, std::string_view update);
std::string encode_bytes(std::string_view bytes);
std::string sanitize_error_detail(std::int64_t code, std::string detail);
std::string load_chats_request(std::int32_t limit);
std::string initialization_request();
std::string qr_login_request();
std::string download_file_request(std::int32_t file_id, std::int32_t priority);
std::string storage_optimizer_request();
std::vector<std::int64_t> decode_integer_array(std::string_view json);
bool chat_leaves_main_list(std::int64_t previous_order, std::int64_t current_order);

}  // namespace telegram::tdlib_test

#endif
