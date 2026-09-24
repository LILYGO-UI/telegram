#include "pages/telegram/telegram_view.hpp"

#include <cassert>
#include <cstdint>

struct TelegramViewVoiceIdentityTestAccess {
    static void request_photo(TelegramView& view, const telegram::Message& message)
    {
        view.request_photo(message);
    }

    static void play_voice(TelegramView& view, const telegram::Message& message)
    {
        view.play_voice(message);
    }

    static bool download_requested(const TelegramView& view, std::int32_t id)
    {
        return view.requested_downloads_.count(id) != 0;
    }

    static bool messages_match(const telegram::Message& left, const telegram::Message& right)
    {
        return TelegramView::voice_identity(left) == TelegramView::voice_identity(right);
    }

    static bool result_matches(const telegram::Message& message, telegram::media::VoiceRequestId active_request_id,
                               std::int64_t result_chat_id, std::int64_t result_message_id,
                               telegram::media::VoiceRequestId result_request_id)
    {
        TelegramView::VoiceResult result;
        result.identity         = {result_chat_id, result_message_id};
        result.event.request_id = result_request_id;
        return TelegramView::voice_result_matches(result, TelegramView::voice_identity(message), active_request_id);
    }
};

int main()
{
    telegram::Message first;
    first.chat_id = 7;
    first.id      = 10;
    first.file.id = 42;

    telegram::Message forwarded = first;
    forwarded.id                = 11;
    assert(first.file.id == forwarded.file.id);
    assert(!TelegramViewVoiceIdentityTestAccess::messages_match(first, forwarded));

    telegram::Message same_message = first;
    assert(TelegramViewVoiceIdentityTestAccess::messages_match(first, same_message));

    assert(TelegramViewVoiceIdentityTestAccess::result_matches(first, 100, 7, 10, 100));
    assert(!TelegramViewVoiceIdentityTestAccess::result_matches(first, 100, 7, 11, 100));
    assert(!TelegramViewVoiceIdentityTestAccess::result_matches(first, 100, 8, 10, 100));
    assert(!TelegramViewVoiceIdentityTestAccess::result_matches(first, 100, 7, 10, 99));
    assert(!TelegramViewVoiceIdentityTestAccess::result_matches(first, 0, 7, 10, 100));

    lv_init();
    {
        telegram::TelegramViewModel model(telegram::make_demo_client(), {});
        assert(model.start());
        model.pump();
        auto preferences = model.preferences();
        preferences.auto_download_photos = false;
        assert(model.set_preferences(preferences));
        TelegramView view(model);
        TelegramViewVoiceIdentityTestAccess::request_photo(view, first);
        assert(!TelegramViewVoiceIdentityTestAccess::download_requested(view, first.file.id));
        TelegramViewVoiceIdentityTestAccess::play_voice(view, first);
        assert(TelegramViewVoiceIdentityTestAccess::download_requested(view, first.file.id));

        preferences.auto_download_photos = true;
        assert(model.set_preferences(preferences));
        first.file.id = 43;
        TelegramViewVoiceIdentityTestAccess::request_photo(view, first);
        assert(TelegramViewVoiceIdentityTestAccess::download_requested(view, first.file.id));
    }
    lv_deinit();
    return 0;
}
