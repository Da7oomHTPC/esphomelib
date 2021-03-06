#include "esphomelib/defines.h"

#ifdef USE_OTA

#include "esphomelib/ota_component.h"
#include "esphomelib/log.h"
#include "esphomelib/esppreferences.h"
#include "esphomelib/helpers.h"
#include "esphomelib/wifi_component.h"
#include "esphomelib/status_led.h"

#include <cstdio>
#ifndef USE_NEW_OTA
  #include <ArduinoOTA.h>
#else
#include <MD5Builder.h>
#ifdef ARDUINO_ARCH_ESP32
  #include <Update.h>
#endif
#endif
#include <StreamString.h>

ESPHOMELIB_NAMESPACE_BEGIN

static const char *TAG = "ota";

#ifdef USE_NEW_OTA
uint8_t OTA_VERSION_1_0 = 1;
#endif

void OTAComponent::setup() {
  this->server_ = new WiFiServer(this->port_);
  this->server_->begin();

#ifdef USE_NEW_OTA

#ifdef ARDUINO_ARCH_ESP32
  add_shutdown_hook([this](const char *cause) {
    this->server_->close();
  });
#endif
#else
  ArduinoOTA.setHostname(global_wifi_component->get_hostname().c_str());
  ArduinoOTA.setPort(this->port_);
  switch (this->auth_type_) {
    case PLAINTEXT: {
      ArduinoOTA.setPassword(this->password_.c_str());
      break;
    }
#if ARDUINO > 20300
    case HASH: {
      ArduinoOTA.setPasswordHash(this->password_.c_str());
      break;
    }
#endif
    case OPEN: {}
    default: break;
  }

  ArduinoOTA.onStart([this]() {
    ESP_LOGI(TAG, "OTA starting...");
    this->ota_triggered_ = true;
    this->at_ota_progress_message_ = 0;
#ifdef ARDUINO_ARCH_ESP8266
    global_preferences.prevent_write(true);
#endif
    this->status_set_warning();
#ifdef USE_STATUS_LED
    global_state |= STATUS_LED_WARNING;
#endif
  });
  ArduinoOTA.onEnd([&]() {
    ESP_LOGI(TAG, "OTA update finished!");
    this->status_clear_warning();
    delay(100);
    run_safe_shutdown_hooks("ota");
  });
  ArduinoOTA.onProgress([this](uint progress, uint total) {
    tick_status_led();
    if (this->at_ota_progress_message_++ % 8 != 0)
      return; // only print every 8th message
    float percentage = float(progress) * 100 / float(total);
    ESP_LOGD(TAG, "OTA in progress: %0.1f%%", percentage);
  });
  ArduinoOTA.onError([this](ota_error_t error) {
    ESP_LOGE(TAG, "Error[%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR: {
        ESP_LOGE(TAG, "  Auth Failed");
        break;
      }
      case OTA_BEGIN_ERROR: {
        ESP_LOGE(TAG, "  Begin Failed");
        break;
      }
      case OTA_CONNECT_ERROR: {
        ESP_LOGE(TAG, "  Connect Failed");
        break;
      }
      case OTA_RECEIVE_ERROR: {
        ESP_LOGE(TAG, "  Receive Failed");
        break;
      }
      case OTA_END_ERROR: {
        ESP_LOGE(TAG, "  End Failed");
        break;
      }
      default:ESP_LOGE(TAG, "  Unknown Error");
    }
    this->ota_triggered_ = false;
    this->status_clear_warning();
    this->status_momentary_error("onerror", 5000);
#ifdef ARDUINO_ARCH_ESP8266
    global_preferences.prevent_write(false);
#endif
  });
  ArduinoOTA.begin();
#endif


  if (this->has_safe_mode_) {
    add_safe_shutdown_hook([this](const char *cause) {
      this->clean_rtc();
    });
  }

  this->dump_config();
}
void OTAComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Over-The-Air Updates:");
  ESP_LOGCONFIG(TAG, "  Address: %s:%u", WiFi.localIP().toString().c_str(), this->port_);
  if (!this->password_.empty()) {
    ESP_LOGCONFIG(TAG, "  Using Password.");
  }
  if (this->has_safe_mode_ && this->safe_mode_rtc_value_ > 1) {
    ESP_LOGW(TAG, "Last Boot was an unhandled reset, will proceed to safe mode in %d restarts",
             this->safe_mode_num_attempts_ - this->safe_mode_rtc_value_);
  }
}

void OTAComponent::loop() {
#ifdef USE_NEW_OTA
  this->handle_();
#else
  do {
    ArduinoOTA.handle();
    tick_status_led();
    yield();
  } while (this->ota_triggered_);
#endif

  if (this->has_safe_mode_ && (millis() - this->safe_mode_start_time_) > this->safe_mode_enable_time_) {
    this->has_safe_mode_ = false;
    // successful boot, reset counter
    ESP_LOGI(TAG, "Boot seems successful, resetting boot loop counter.");
    this->clean_rtc();
  }
}

#ifdef USE_NEW_OTA
void OTAComponent::handle_() {
  OTAResponseTypes error_code = OTA_RESPONSE_ERROR_UNKNOWN;
  bool update_started = false;
  uint32_t total = 0;
  uint32_t last_progress = 0;
  uint8_t buf[1024];
  char *sbuf = reinterpret_cast<char *>(buf);
  uint32_t ota_size;
  uint8_t ota_features;

  if (!this->client_.connected()) {
    this->client_ = this->server_->available();

    if (!this->client_.connected())
      return;
  }

  // enable nodelay for outgoing data
  this->client_.setNoDelay(true);

  ESP_LOGD(TAG, "Starting OTA Update from %s...", this->client_.remoteIP().toString().c_str());
  this->status_set_warning();
#ifdef USE_STATUS_LED
  global_state |= STATUS_LED_WARNING;
#endif

  if (!this->wait_receive_(buf, 5)) {
    ESP_LOGW(TAG, "Reading magic bytes failed!");
    goto error;
  }
  // 0x6C, 0x26, 0xF7, 0x5C, 0x45
  if (buf[0] != 0x6C || buf[1] != 0x26 || buf[2] != 0xF7 || buf[3] != 0x5C || buf[4] != 0x45) {
    ESP_LOGW(TAG, "Magic bytes do not match! 0x%02X-0x%02X-0x%02X-0x%02X-0x%02X",
             buf[0], buf[1], buf[2], buf[3], buf[4]);
    error_code = OTA_RESPONSE_ERROR_MAGIC;
    goto error;
  }

  // Send OK and version - 2 bytes
  this->client_.write(OTA_RESPONSE_OK);
  this->client_.write(OTA_VERSION_1_0);

  // Read features - 1 byte
  if (!this->wait_receive_(buf, 1)) {
    ESP_LOGW(TAG, "Reading features failed!");
    goto error;
  }
  ota_features = buf[0];
  ESP_LOGV(TAG, "OTA features is 0x%02X", ota_features);

  // Acknowledge header - 1 byte
  this->client_.write(OTA_RESPONSE_HEADER_OK);

  if (!this->password_.empty()) {
    this->client_.write(OTA_RESPONSE_REQUEST_AUTH);
    MD5Builder md5_builder{};
    md5_builder.begin();
    sprintf(sbuf, "%08X", random_uint32());
    md5_builder.add(sbuf);
    md5_builder.calculate();
    md5_builder.getChars(sbuf);
    ESP_LOGV(TAG, "Auth: Nonce is %s", sbuf);

    // Send nonce, 32 bytes hex MD5
    if (this->client_.write(sbuf, 32) != 32) {
      ESP_LOGW(TAG, "Auth: Writing nonce failed!");
      goto error;
    }

    // prepare challenge
    md5_builder.begin();
    md5_builder.add(this->password_.c_str());
    // add nonce
    md5_builder.add(sbuf);

    // Receive cnonce, 32 bytes hex MD5
    if (!this->wait_receive_(buf, 32)) {
      ESP_LOGW(TAG, "Auth: Reading cnonce failed!");
      goto error;
    }
    sbuf[32] = '\0';
    ESP_LOGV(TAG, "Auth: CNonce is %s", sbuf);
    // add cnonce
    md5_builder.add(sbuf);

    // calculate result
    md5_builder.calculate();
    md5_builder.getChars(sbuf);
    ESP_LOGV(TAG, "Auth: Result is %s", sbuf);

    // Receive result, 32 bytes hex MD5
    if (!this->wait_receive_(buf + 64, 32)) {
      ESP_LOGW(TAG, "Auth: Reading response failed!");
      goto error;
    }
    sbuf[64 + 32] = '\0';
    ESP_LOGV(TAG, "Auth: Response is %s", sbuf + 64);

    bool matches = true;
    for (uint8_t i = 0; i < 32; i++)
      matches = matches && buf[i] == buf[64 + i];

    if (!matches) {
      ESP_LOGW(TAG, "Auth failed! Passwords do not match!");
      error_code = OTA_RESPONSE_ERROR_AUTH_INVALID;
      goto error;
    }
  }

  // Acknowledge auth OK - 1 byte
  this->client_.write(OTA_RESPONSE_AUTH_OK);

  // Read size, 4 bytes MSB first
  if (!this->wait_receive_(buf, 4)) {
    ESP_LOGW(TAG, "Reading size failed!");
    goto error;
  }
  ota_size = 0;
  for (uint8_t i = 0; i < 4; i++) {
    ota_size <<= 8;
    ota_size |= buf[i];
  }
  ESP_LOGV(TAG, "OTA size is %u bytes", ota_size);

#ifdef ARDUINO_ARCH_ESP8266
  global_preferences.prevent_write(true);
#endif

  if (!Update.begin(ota_size, U_FLASH)) {
#ifdef ARDUINO_ARCH_ESP8266
    StreamString ss;
    Update.printError(ss);
    if (ss.indexOf("Invalid bootstrapping") != -1) {
      error_code = OTA_RESPONSE_ERROR_INVALID_BOOTSTRAPPING;
      goto error;
    }
#endif
    ESP_LOGW(TAG, "Preparing OTA partition failed! Is the binary too big?");
    error_code = OTA_RESPONSE_ERROR_UPDATE_PREPARE;
    goto error;
  }
  update_started = true;

  // Acknowledge prepare OK - 1 byte
  this->client_.write(OTA_RESPONSE_UPDATE_PREPARE_OK);

  // Read binary MD5, 32 bytes
  if (!this->wait_receive_(buf, 32)) {
    ESP_LOGW(TAG, "Reading binary MD5 checksum failed!");
    goto error;
  }
  sbuf[32] = '\0';
  ESP_LOGV(TAG, "Update: Binary MD5 is %s", sbuf);
  Update.setMD5(sbuf);

  // Acknowledge MD5 OK - 1 byte
  this->client_.write(OTA_RESPONSE_BIN_MD5_OK);

  while (!Update.isFinished()) {
    size_t available = this->wait_receive_(buf, 0);
    if (!available) {
      goto error;
    }

    uint32_t written = Update.write(buf, available);
    if (written != available) {
      ESP_LOGW(TAG, "Error writing binary data to flash: %u != %d!", written, available);
      error_code = OTA_RESPONSE_ERROR_WRITING_FLASH;
      goto error;
    }
    total += written;

    uint32_t now = millis();
    if (now - last_progress > 1000) {
      last_progress = now;
      float percentage = (total * 100.0f) / ota_size;
      ESP_LOGD(TAG, "OTA in progress: %0.1f%%", percentage);
    }
  }

  // Acknowledge receive OK - 1 byte
  this->client_.write(OTA_RESPONSE_RECEIVE_OK);

  if (!Update.end()) {
    error_code = OTA_RESPONSE_ERROR_UPDATE_END;
    goto error;
  }

  // Acknowledge Update end OK - 1 byte
  this->client_.write(OTA_RESPONSE_UPDATE_END_OK);

  // Read ACK
  if (!this->wait_receive_(buf, 1, false) || buf[0] != OTA_RESPONSE_OK) {
    ESP_LOGW(TAG, "Reading back acknowledgement failed!");
    // do not go to error, this is not fatal
  }

  this->client_.flush();
  this->client_.stop();
  delay(10);
  ESP_LOGI(TAG, "OTA update finished!");
  this->status_clear_warning();
  delay(100);
  safe_reboot("ota");

  error:
  if (update_started) {
    StreamString ss;
    Update.printError(ss);
    ESP_LOGW(TAG, "Update end failed! Error: %s", ss.c_str());
  }
  if (this->client_.connected()) {
    this->client_.write(error_code);
    this->client_.flush();
  }
  this->client_.stop();
#ifdef ARDUINO_ARCH_ESP32
  if (update_started) {
    Update.abort();
  }
#endif
  this->status_momentary_error("onerror", 5000);
#ifdef ARDUINO_ARCH_ESP8266
  global_preferences.prevent_write(false);
#endif
}

size_t OTAComponent::wait_receive_(uint8_t *buf, size_t bytes, bool check_disconnected) {
  size_t available = 0;
  uint32_t start = millis();
  do {
    tick_status_led();
    if (check_disconnected && !this->client_.connected()) {
      ESP_LOGW(TAG, "Error client disconnected while receiving data!");
      return 0;
    }
    int availi = this->client_.available();
    if (availi < 0) {
      ESP_LOGW(TAG, "Error reading data!");
      return 0;
    }
    uint32_t now = millis();
    if (availi == 0 && now - start > 10000) {
      ESP_LOGW(TAG, "Timeout waiting for data!");
      return 0;
    }
    available = size_t(availi);
    yield();
  } while (bytes == 0 ? available == 0 : available < bytes);

  if (bytes == 0)
    bytes = std::min(available, size_t(1024));

  int res = this->client_.read(buf, bytes);

  if (res != int(bytes)) {
    ESP_LOGW(TAG, "Error reading binary data: %d (%u)!", res, bytes);
    return 0;
  }

  return bytes;
}
#endif

OTAComponent::OTAComponent(uint16_t port)
    : port_(port) {

}

#ifdef USE_NEW_OTA
void OTAComponent::set_auth_password(const std::string &password) {
  this->password_ = password;
}
#else
void OTAComponent::set_auth_plaintext_password(const std::string &password) {
  this->auth_type_ = PLAINTEXT;
  this->password_ = password;
}
void OTAComponent::set_auth_password_hash(const std::string &hash) {
  this->auth_type_ = HASH;
  this->password_ = hash;
}
#endif

float OTAComponent::get_setup_priority() const {
  return setup_priority::MQTT_CLIENT + 1.0f;
}
uint16_t OTAComponent::get_port() const {
  return this->port_;
}
void OTAComponent::set_port(uint16_t port) {
  this->port_ = port;
}
void OTAComponent::start_safe_mode(uint8_t num_attempts, uint32_t enable_time) {
  this->has_safe_mode_ = true;
  this->safe_mode_start_time_ = millis();
  this->safe_mode_enable_time_ = enable_time;
  this->safe_mode_num_attempts_ = num_attempts;
  this->rtc_ = global_preferences.make_preference<uint8_t>(669657188UL);
  this->safe_mode_rtc_value_ = this->read_rtc_();

  ESP_LOGCONFIG(TAG, "There have been %u suspected unsuccessful boot attempts.", this->safe_mode_rtc_value_);

  if (this->safe_mode_rtc_value_ >= num_attempts) {
    this->clean_rtc();

    ESP_LOGE(TAG, "Boot loop detected. Proceeding to safe mode.");

#ifdef USE_STATUS_LED
    if (global_status_led != nullptr) {
      global_status_led->setup_();
    }
#endif
    global_state = STATUS_LED_ERROR;
    global_wifi_component->setup_();
    while (!global_wifi_component->ready_for_ota()) {
      yield();
      global_wifi_component->loop_();
      tick_status_led();
    }
    this->setup_();

    ESP_LOGI(TAG, "Waiting for OTA attempt.");
    uint32_t begin = millis();
    while ((millis() - begin) < enable_time) {
      this->loop_();
      global_wifi_component->loop_();
      yield();
    }
    ESP_LOGE(TAG, "No OTA attempt made, restarting.");
    reboot("ota-safe-mode");
  } else {
    // increment counter
    this->write_rtc_(uint8_t(this->safe_mode_rtc_value_ + 1));
  }
}
void OTAComponent::write_rtc_(uint8_t val) {
  this->rtc_.save(&val);
}
uint8_t OTAComponent::read_rtc_() {
  uint8_t val;
  if (!this->rtc_.load(&val))
    return 0;
  return val;
}
void OTAComponent::clean_rtc() {
  this->write_rtc_(0);
}

ESPHOMELIB_NAMESPACE_END

#endif //USE_OTA
