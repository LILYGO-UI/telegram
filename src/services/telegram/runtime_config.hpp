#ifndef LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_RUNTIME_CONFIG_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_RUNTIME_CONFIG_HPP

#include "services/telegram/telegram_client.hpp"

#include <string>

namespace telegram {

struct RuntimeConfig {
    ClientConfig client;
    bool demo = false;
    std::string demo_image_path;
};

RuntimeConfig load_runtime_config();

}  // namespace telegram

#endif
