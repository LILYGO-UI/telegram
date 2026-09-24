#ifndef LILYGO_UI_TELEGRAM_TDLIB_ACCOUNT_PROTOCOL_HPP
#define LILYGO_UI_TELEGRAM_TDLIB_ACCOUNT_PROTOCOL_HPP

#include "domain/telegram_account.hpp"
#include "support/json.hpp"

#include <functional>
#include <memory>

namespace telegram {

// Runs only on the TDLib worker. Wire IDs are independent of UI request IDs.
class TdlibAccountProtocol {
public:
    using Json = support::json::Value;
    using Send = std::function<bool(Json, std::uint64_t)>;
    using Emit = std::function<void(AccountUpdate)>;
    using ParseChat = std::function<Chat(const Json&)>;
    using Clock = std::function<std::int64_t()>;

    TdlibAccountProtocol(Send send, Emit emit, ParseChat parse_chat, Clock clock = {});
    ~TdlibAccountProtocol();
    void submit(AccountRequest request);
    void response(std::uint64_t wire_id, const Json& value);
    void observe(const Json& update);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telegram
#endif
