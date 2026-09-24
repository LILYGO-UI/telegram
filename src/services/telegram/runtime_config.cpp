#include "services/telegram/runtime_config.hpp"

#include "telegram_config.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace telegram {
namespace {

const char* environment(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

std::int32_t parse_api_id(const char* value, std::int32_t fallback)
{
    if (!value) return fallback;
    errno          = 0;
    char* end      = nullptr;
    const long raw = std::strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || raw <= 0 || raw > INT_MAX) return fallback;
    return static_cast<std::int32_t>(raw);
}

bool parse_bool(const char* value, bool fallback)
{
    if (!value) return fallback;
    const std::string_view text(value);
    if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
    if (text == "0" || text == "false" || text == "no" || text == "off") return false;
    return fallback;
}

std::filesystem::path data_root()
{
    if (const char* xdg = environment("XDG_DATA_HOME")) return xdg;
    if (const char* home = environment("HOME")) return std::filesystem::path(home) / ".local" / "share";
    return std::filesystem::temp_directory_path();
}

}  // namespace

RuntimeConfig load_runtime_config()
{
    RuntimeConfig result;
    result.client.api_id = parse_api_id(environment("TELEGRAM_API_ID"), LILYGO_UI_TELEGRAM_API_ID);
    result.client.api_hash =
        environment("TELEGRAM_API_HASH") ? environment("TELEGRAM_API_HASH") : LILYGO_UI_TELEGRAM_API_HASH;
    result.client.database_encryption_key =
        environment("TELEGRAM_DATABASE_KEY") ? environment("TELEGRAM_DATABASE_KEY") : "";
    result.client.system_language_code = environment("LANG") ? environment("LANG") : "en";

    const auto root                  = data_root() / "lilygo-ui-telegram";
    result.client.database_directory = (root / "database").string();
    result.client.files_directory    = (root / "files").string();
    result.client.preferences_path   = (root / "ui-preferences.json").string();
    result.demo                      = parse_bool(environment("TELEGRAM_DEMO"), LILYGO_UI_TELEGRAM_DEMO != 0);
#if defined(LILYGO_UI_TELEGRAM_DEMO_IMAGE)
    result.demo_image_path = LILYGO_UI_TELEGRAM_DEMO_IMAGE;
#endif
    if (const char* demo_image = environment("TELEGRAM_DEMO_IMAGE")) result.demo_image_path = demo_image;
    return result;
}

}  // namespace telegram
