#include "app.hpp"

#include "app_identity.h"
#include "pages/telegram/telegram_view.hpp"
#include "services/telegram/runtime_config.hpp"
#include "services/telegram/telegram_client.hpp"

#include <memory>
#include <utility>

namespace {

struct AppState {
    std::unique_ptr<telegram::TelegramViewModel> view_model;
    std::unique_ptr<TelegramView> view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    auto runtime_config = telegram::load_runtime_config();
    auto client         = runtime_config.demo ? telegram::make_demo_client(runtime_config.demo_image_path)
                                              : telegram::make_tdlib_client();
    app_state.view_model =
        std::make_unique<telegram::TelegramViewModel>(std::move(client), std::move(runtime_config.client));
    app_state.view = std::make_unique<TelegramView>(*app_state.view_model);
    app_state.view->create(context->root, LILYGO_UI_TELEGRAM_APP_NAME);
    app_state.view_model->start();
}

void app_close()
{
    if (app_state.view) app_state.view->destroy();
    app_state.view.reset();
    if (app_state.view_model) app_state.view_model->stop();
    app_state.view_model.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t), LILYGO_UI_TELEGRAM_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
