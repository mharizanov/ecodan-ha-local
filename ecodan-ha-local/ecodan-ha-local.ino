#include <WiFi.h>
#include <time.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "ehal.h"
#include "ehal_config.h"
#include "ehal_diagnostics.h"
#include "ehal_hp.h"
#include "ehal_http.h"
#include "ehal_mqtt.h"
#include "ehal_thirdparty.h"

bool mqttInitialized = false;
bool heatpumpInitialized = false;
uint8_t ledTick = 0;
const uint8_t ledTickPatternHpDisconnect[] = { HIGH, LOW, HIGH, LOW, HIGH, HIGH, HIGH, HIGH, LOW };
const uint8_t ledTickPatternMqttDisconnect[] = { HIGH, HIGH, HIGH, LOW, LOW, LOW };
auto since_start = std::chrono::steady_clock::now();

bool initialize_wifi_access_point()
{
    ehal::log_web(F("Initializing WiFi connection..."));

    ehal::Config& config = ehal::config_instance();

    if (!ehal::requires_first_time_configuration())
    {
        if (!config.HostName.isEmpty())
        {
            if (!WiFi.setHostname(config.HostName.c_str()))
            {
                ehal::log_web(F("Failed to configure hostname from saved settings!"));
            }
        }
        WiFi.begin(config.WifiSsid.c_str(), config.WifiPassword.c_str());

        // Give us few mins to re-establish a WiFi connection before we give up,
        // just in case there's a power cut and the router needs time to boot.
        for (int i = 0; i < 800; ++i)
        {
            if (WiFi.isConnected())
                break;

            ehal::log_web(F("Waiting 500ms for WiFi connection..."));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (!WiFi.isConnected())
        {
            ehal::log_web(F("Couldn't connect to WiFi network, restarting."));
            /* 
            config.WifiPassword.clear();
            config.WifiSsid.clear();
            ehal::save_configuration(config);
            */
            ESP.restart();
        }
  
    }

    if (ehal::requires_first_time_configuration())
    {
        if (!WiFi.softAP(config.HostName.c_str(), config.WifiPassword.c_str()))
        {
            ehal::log_web(F("Unable to create WiFi Access point!"));
            return false;
        }
    }

    WiFi.setAutoReconnect(true);

    ehal::log_web(F("WiFi connection established!"));
    return true;
}

void update_time(bool force)
{
    static auto last_time_update = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    if (force || now - last_time_update > (std::chrono::hours(24) + std::chrono::seconds(5)))
    {
        last_time_update = now;
        configTzTime("UTC0", "pool.ntp.org");
        ehal::log_web(F("Updated UTC time from NTP"));
    }
}

void set_boot_time()
{
    auto& config = ehal::config_instance();

    const size_t len = 21; // "yyyy-mm-ddThh:mm:ssZ\0"
    auto buffer = std::unique_ptr<char[]>(new char[len]);
    time_t now = time(nullptr);
    struct tm t = *localtime(&now);

    if (strftime(buffer.get(), len, "%FT%TZ", &t) == 0)
        return;

    config.BootTime = String(buffer.get());
}

void update_status_led()
{
    static auto last_tick_update = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();

    if (now - last_tick_update > std::chrono::milliseconds(250))
    {
        last_tick_update = now;

        auto& config = ehal::config_instance();

        if (!ehal::hp::is_connected())
        {
            digitalWrite(config.StatusLed, ledTickPatternHpDisconnect[ledTick++ % sizeof(ledTickPatternHpDisconnect)]);
        }
        else if (!ehal::mqtt::is_connected())
        {
            digitalWrite(config.StatusLed, ledTickPatternMqttDisconnect[ledTick++ % sizeof(ledTickPatternMqttDisconnect)]);
        }
        else
        {
            digitalWrite(config.StatusLed, HIGH);
        }
    }
}

void log_last_reset_reason()
{
    auto reason = esp_reset_reason();
    switch (reason)
    {
        case ESP_RST_POWERON:
            ehal::log_web(F("Reset due to power-on event."));
            break;
        case ESP_RST_SW:
            ehal::log_web(F("Software reset via esp_restart."));
            break;
        case ESP_RST_PANIC:
            ehal::log_web(F("Software reset due to exception/panic."));
            break;
        case ESP_RST_INT_WDT:
            ehal::log_web(F("Reset (software or hardware) due to interrupt watchdog."));
            break;
        case ESP_RST_TASK_WDT:
            ehal::log_web(F("Reset due to task watchdog."));
            break;
        case ESP_RST_WDT:
            ehal::log_web(F("Reset due to other watchdogs."));
            break;
        case ESP_RST_DEEPSLEEP:
            ehal::log_web(F("Reset after exiting deep sleep mode."));
            break;
        case ESP_RST_BROWNOUT:
            ehal::log_web(F("Brownout reset (software or hardware)."));
            break;
        case ESP_RST_SDIO:
            ehal::log_web(F("Reset over SDIO."));
            break;
        default:
            ehal::log_web(F("Reset for unknown reason (%d)"), reason);
            break;
    }
}

void setup()
{
    if (!ehal::load_saved_configuration())
    {
        ehal::log_web(F("Failed to load configuration!"));
        return;
    }

    ehal::log_web(F("Configuration parameters loaded from NVS"));

    initialize_wifi_access_point();

    update_time(/* force =*/true);
    set_boot_time();

    if (ehal::requires_first_time_configuration())
    {
        ehal::log_web(F("First time configuration required, starting captive portal..."));

        ehal::http::initialize_captive_portal();
    }
    else
    {
        ehal::http::initialize_default();
        heatpumpInitialized = ehal::hp::initialize();
        mqttInitialized = ehal::mqtt::initialize();
    }

    pinMode(ehal::config_instance().StatusLed, OUTPUT);

    log_last_reset_reason();
    ehal::log_web(F("Ecodan HomeAssistant Bridge startup successful, starting request processing."));

    ehal::init_watchdog();
    ehal::add_thread_to_watchdog();
}

void loop()
{
    auto now = std::chrono::steady_clock::now();

    if (now - since_start > std::chrono::hours(24))
    {
      ehal::log_web(F("24h uptime timer expired - restarting.."));
      ESP.restart();
    }

    try
    {
        ehal::ping_watchdog();

        ehal::http::handle_loop();

        if (heatpumpInitialized)
            ehal::hp::handle_loop();

        if (mqttInitialized)
            ehal::mqtt::handle_loop();

        if (!WiFi.isConnected())
        {
            ehal::log_web(F("WiFi connection lost, attempting reconnect"));
            initialize_wifi_access_point();
        }

        update_time(/* force =*/false);
        update_status_led();
    }
    catch (std::exception const& ex)
    {
        ehal::log_web(F("Exception occurred during main loop processing: %s"), ex.what());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
