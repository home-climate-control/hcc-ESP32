#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "esp_log.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#include "onewire.h"

namespace hcc_onewire {

#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD_MILLIS        (1000 * CONFIG_ONE_WIRE_POLL_SECONDS)

int OneWire::browse()
{

    // VT: FIXME: Wrap this into a mutex at some point.

    // To debug OWB, use 'make menuconfig' to set default Log level to DEBUG, then uncomment:
    //esp_log_level_set("owb", ESP_LOG_DEBUG);

    // Stable readings require a brief period before communication
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus, using the RMT timeslot driver
    owb = owb_rmt_initialize(&rmt_driver_info, gpioOnewire, RMT_CHANNEL_1, RMT_CHANNEL_0);

    // enable CRC check for ROM code
    owb_use_crc(owb, true);

    ESP_LOGI(TAG, "[1-Wire] looking for connected devices on pin %d...", gpioOnewire);

    std::vector<OneWireBus_ROMCode> device_rom_codes;
    OneWireBus_SearchState search_state = {};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    int devices_found = 0;
    while (found) {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        strupr(rom_code_s);
        ESP_LOGI(TAG, "[1-Wire] %d: %s", devices_found, rom_code_s);
        device_rom_codes.push_back(search_state.rom_code);
        addresses.push_back(rom_code_s);

        ++devices_found;
        owb_search_next(owb, &search_state, &found);
    }
    ESP_LOGI(TAG, "[1-Wire] found %d device%s on pin %d", devices_found, devices_found == 1 ? "" : "s", gpioOnewire);

    // Create DS18B20 devices on the 1-Wire bus
    for (int i = 0; i < devices_found; ++i) {
        DS18B20_Info *ds18b20_info = ds18b20_malloc();
        devices.push_back(ds18b20_info);

        if (devices_found == 1) {
            ESP_LOGD(TAG, "[1-Wire] single device optimizations enabled");
            ds18b20_init_solo(ds18b20_info, owb);
        } else {
            // associate with bus and device
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]);
        }

        // enable CRC check for temperature readings
        ds18b20_use_crc(ds18b20_info, true);
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    devicesFound = devices_found;

    return devicesFound;
}

/**
 * Turn the LED on for the time specified in menuconfig, then turn it off.
 */
void OneWire::flashLED()
{

    if (gpioLED == GPIO_NUM_NC) {
        return;
    }

    gpio_pad_select_gpio(gpioLED);
    gpio_set_direction(gpioLED, GPIO_MODE_OUTPUT);

    gpio_set_level(gpioLED, 1);

    const TickType_t delayMillis = flashMillis / portTICK_PERIOD_MS;
    vTaskDelay( delayMillis );

    gpio_set_level(gpioLED, 0);
}

std::vector<float> OneWire::poll()
{

    flashLED();

    float readings[devices.size()] = { 0 };
    DS18B20_ERROR errors[devices.size()] = {};

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    ds18b20_convert_all(owb);

    // In this application all devices use the same resolution,
    // so use the first device to determine the delay
    ds18b20_wait_for_conversion(devices[0]);

    // Read the results immediately after conversion otherwise it may fail
    // (using printf before reading may take too long)
    for (int offset = 0; offset < devicesFound; ++offset) {
        errors[offset] = ds18b20_read_temp(devices[offset], &readings[offset]);
    }

    int errors_count[devices.size()] = {0};
    std::vector<float> result;

    // Report results in a separate loop, after all have been read
    for (int offset = 0; offset < devicesFound; ++offset) {
        if (errors[offset] != DS18B20_OK) {
            // VT: NOTE: What exactly does the error count indicate here? Need to RTFM again.
            ++errors_count[offset];
        }

        // VT: FIXME: This will not handle errors correctly
        result.push_back(readings[offset]);
    }

    flashLED();

    return result;
}
}
