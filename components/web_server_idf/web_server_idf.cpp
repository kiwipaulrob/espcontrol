#ifdef USE_ESP32

#include <cstdarg>
#include <memory>
#include <cstring>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/defines.h"

#include "esp_tls_crypto.h"
#include "esp_partition.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "utils.h"
#include "web_server_idf.h"

#ifdef USE_WEBSERVER_OTA
#include <multipart_parser.h>
#include "multipart.h"  // For parse_multipart_boundary and other utils
#endif

#ifdef USE_WEBSERVER
#include "esphome/components/web_server/web_server.h"
#include "esphome/components/web_server/list_entities.h"
#endif  // USE_WEBSERVER

// Include socket headers after Arduino headers to avoid IPADDR_NONE/INADDR_NONE macro conflicts
#include <cerrno>
#include <sys/socket.h>

namespace esphome::web_server_idf {

#ifndef HTTPD_409
#define HTTPD_409 "409 Conflict"
#endif

#define CRLF_STR "\r\n"
#define CRLF_LEN (sizeof(CRLF_STR) - 1)

static const char *const TAG = "web_server_idf";
static constexpr size_t CARD_IMAGE_MAX_BYTES = 64 * 1024;
static constexpr size_t CARD_IMAGE_NAME_MAX_LENGTH = 40;
static constexpr size_t CARD_IMAGE_FLASH_SECTOR_SIZE = 4096;
static constexpr uint32_t CARD_IMAGE_MAGIC = 0x43494D47;  // "CIMG"
static constexpr uint32_t CARD_IMAGE_VERSION = 1;

// Global instance to avoid guard variable (saves 8 bytes)
// This is initialized at program startup before any threads
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DefaultHeaders default_headers_instance;
}  // namespace

DefaultHeaders &DefaultHeaders::Instance() { return default_headers_instance; }

namespace {
#ifdef ESPHOME_PROJECT_NAME
static constexpr const char *ESPCONTROL_PROJECT_NAME = ESPHOME_PROJECT_NAME;
#else
static constexpr const char *ESPCONTROL_PROJECT_NAME = "";
#endif

#ifdef ESPHOME_PROJECT_VERSION
static constexpr const char *ESPCONTROL_PROJECT_VERSION = ESPHOME_PROJECT_VERSION;
#else
static constexpr const char *ESPCONTROL_PROJECT_VERSION = "";
#endif

void append_json_string(std::string &out, const char *value) {
  out.push_back('"');
  for (const char *p = value; p != nullptr && *p != '\0'; ++p) {
    switch (*p) {
      case '\\':
      case '"':
        out.push_back('\\');
        out.push_back(*p);
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(*p);
        break;
    }
  }
  out.push_back('"');
}

std::string firmware_version_json() {
  std::string out;
  out.reserve(128);
  out.append("{\"project_name\":");
  append_json_string(out, ESPCONTROL_PROJECT_NAME);
  out.append(",\"project_version\":");
  append_json_string(out, ESPCONTROL_PROJECT_VERSION);
  out.append(",\"firmware_version\":");
  append_json_string(out, ESPCONTROL_PROJECT_VERSION);
  out.append(",\"version\":");
  append_json_string(out, ESPCONTROL_PROJECT_VERSION);
  out.push_back('}');
  return out;
}

bool handle_firmware_version_request(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_GET) {
    return false;
  }
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  if (url != "/espcontrol/version" && url != "/espcontrol/version.json") {
    return false;
  }
  std::string body = firmware_version_json();
  request->send(200, "application/json", body.c_str());
  return true;
}

void apply_no_cache_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
}

bool is_loopback_request(httpd_req_t *r) {
  int fd = httpd_req_to_sockfd(r);
  if (fd < 0) return false;
  sockaddr_storage addr {};
  socklen_t addr_len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) return false;
  if (addr.ss_family == AF_INET) {
    auto *in = reinterpret_cast<sockaddr_in *>(&addr);
    return ntohl(in->sin_addr.s_addr) == INADDR_LOOPBACK;
  }
  return false;
}

bool card_image_id_valid(const std::string &id) {
  if (id.empty() || id.size() > 40) return false;
  for (char ch : id) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
  }
  return true;
}

std::string card_image_id_from_url(const std::string &url, const char *prefix) {
  std::string rest = url.substr(strlen(prefix));
  if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".jpg") == 0) {
    rest.resize(rest.size() - 4);
  }
  return card_image_id_valid(rest) ? rest : "";
}

struct CardImageHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t size;
  uint32_t reserved;
  char id[48];
  char name[48];
  uint8_t padding[16];
};

static_assert(sizeof(CardImageHeader) == 128, "Card image flash header must remain one flash-aligned block");

const esp_partition_t *card_image_partition() {
  static const esp_partition_t *partition = nullptr;
  static bool attempted = false;
  if (attempted) return partition;
  attempted = true;
  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (partition == nullptr) {
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  }
  if (partition == nullptr) {
    ESP_LOGW(TAG, "Card image storage partition not found");
  } else if (partition->size < CARD_IMAGE_FLASH_SECTOR_SIZE) {
    ESP_LOGW(TAG, "Card image storage partition is too small: %u bytes", static_cast<unsigned>(partition->size));
    partition = nullptr;
  } else {
    ESP_LOGI(TAG, "Using card image storage partition '%s' at 0x%08" PRIx32 " (%u bytes)",
             partition->label, partition->address, static_cast<unsigned>(partition->size));
  }
  return partition;
}

size_t card_image_record_size(size_t image_size) {
  size_t bytes = sizeof(CardImageHeader) + image_size;
  return ((bytes + CARD_IMAGE_FLASH_SECTOR_SIZE - 1) / CARD_IMAGE_FLASH_SECTOR_SIZE) * CARD_IMAGE_FLASH_SECTOR_SIZE;
}

bool card_image_header_valid(CardImageHeader &header) {
  if (header.magic != CARD_IMAGE_MAGIC || header.version != CARD_IMAGE_VERSION) return false;
  if (header.size == 0 || header.size > CARD_IMAGE_MAX_BYTES) return false;
  header.id[sizeof(header.id) - 1] = '\0';
  header.name[sizeof(header.name) - 1] = '\0';
  if (!card_image_id_valid(header.id)) return false;
  return true;
}

bool read_card_image_header_at(size_t offset, CardImageHeader &header) {
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr || offset + sizeof(header) > partition->size) return false;
  if (esp_partition_read(partition, offset, &header, sizeof(header)) != ESP_OK) return false;
  return card_image_header_valid(header);
}

std::string normalize_card_image_name(const std::string &value) {
  std::string out;
  out.reserve(std::min(value.size(), CARD_IMAGE_NAME_MAX_LENGTH));
  bool previous_space = false;
  for (char raw : value) {
    unsigned char ch = static_cast<unsigned char>(raw);
    if (ch < 0x20 || ch == 0x7F) continue;
    if (raw == ',' || raw == ';') continue;
    if (std::isspace(ch)) {
      if (!out.empty() && !previous_space) {
        out.push_back(' ');
        previous_space = true;
      }
      continue;
    }
    out.push_back(raw);
    previous_space = false;
    if (out.size() >= CARD_IMAGE_NAME_MAX_LENGTH) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string card_image_display_name(const CardImageHeader &header) {
  std::string name = normalize_card_image_name(header.name);
  return name.empty() ? std::string(header.id) : name;
}

std::string card_image_item_json(const CardImageHeader &header) {
  std::string body = "{\"id\":";
  append_json_string(body, header.id);
  body += ",\"name\":";
  append_json_string(body, card_image_display_name(header).c_str());
  body += ",\"size\":";
  body += std::to_string(header.size);
  body += ",\"url\":\"/card-images/";
  body += header.id;
  body += ".jpg\"}";
  return body;
}

int find_card_image_offset(const std::string &id) {
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) return -1;
  CardImageHeader header {};
  for (size_t offset = 0; offset + sizeof(header) <= partition->size; offset += CARD_IMAGE_FLASH_SECTOR_SIZE) {
    if (read_card_image_header_at(offset, header) && id == header.id) return static_cast<int>(offset);
  }
  return -1;
}

esp_err_t update_card_image_name(const std::string &id, const std::string &name, CardImageHeader &header) {
  int offset = find_card_image_offset(id);
  const esp_partition_t *partition = card_image_partition();
  if (offset < 0 || partition == nullptr || !read_card_image_header_at(static_cast<size_t>(offset), header)) {
    return ESP_ERR_NOT_FOUND;
  }
  strlcpy(header.name, name.c_str(), sizeof(header.name));
  size_t record_offset = static_cast<size_t>(offset);
  std::unique_ptr<uint8_t[]> sector(new uint8_t[CARD_IMAGE_FLASH_SECTOR_SIZE]);
  esp_err_t err = esp_partition_read(partition, record_offset, sector.get(), CARD_IMAGE_FLASH_SECTOR_SIZE);
  if (err == ESP_OK) {
    memcpy(sector.get(), &header, sizeof(header));
    err = esp_partition_erase_range(partition, record_offset, CARD_IMAGE_FLASH_SECTOR_SIZE);
  }
  if (err == ESP_OK) {
    err = esp_partition_write(partition, record_offset, sector.get(), CARD_IMAGE_FLASH_SECTOR_SIZE);
  }
  return err;
}

size_t card_image_used_bytes() {
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) return 0;
  size_t used = 0;
  CardImageHeader header {};
  for (size_t offset = 0; offset + sizeof(header) <= partition->size; offset += CARD_IMAGE_FLASH_SECTOR_SIZE) {
    if (!read_card_image_header_at(offset, header)) continue;
    size_t record_size = card_image_record_size(header.size);
    if (offset + record_size <= partition->size) used += record_size;
  }
  return used;
}

int find_empty_card_image_offset(size_t image_size) {
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) return -1;
  size_t required_sectors = card_image_record_size(image_size) / CARD_IMAGE_FLASH_SECTOR_SIZE;
  size_t total_sectors = partition->size / CARD_IMAGE_FLASH_SECTOR_SIZE;
  if (required_sectors == 0 || required_sectors > total_sectors) return -1;
  std::vector<uint8_t> used(total_sectors, 0);
  CardImageHeader header {};
  for (size_t sector = 0; sector < total_sectors; sector++) {
    size_t offset = sector * CARD_IMAGE_FLASH_SECTOR_SIZE;
    if (!read_card_image_header_at(offset, header)) continue;
    size_t record_sectors = card_image_record_size(header.size) / CARD_IMAGE_FLASH_SECTOR_SIZE;
    if (record_sectors == 0 || sector + record_sectors > total_sectors) continue;
    for (size_t i = 0; i < record_sectors; i++) used[sector + i] = 1;
  }
  size_t run = 0;
  size_t start = 0;
  for (size_t sector = 0; sector < total_sectors; sector++) {
    if (used[sector]) {
      run = 0;
      start = sector + 1;
      continue;
    }
    if (run == 0) start = sector;
    run++;
    if (run >= required_sectors) {
      return static_cast<int>(start * CARD_IMAGE_FLASH_SECTOR_SIZE);
    }
  }
  return -1;
}

std::string card_image_list_json() {
  const esp_partition_t *partition = card_image_partition();
  size_t storage_bytes = partition ? partition->size : 0;
  size_t used_bytes = card_image_used_bytes();
  size_t free_bytes = storage_bytes > used_bytes ? storage_bytes - used_bytes : 0;
  std::string out = "{\"available\":";
  out += partition ? "true" : "false";
  out += ",\"requires_usb_flash\":";
  out += partition ? "false" : "true";
  out += ",\"storage_bytes\":";
  out += std::to_string(storage_bytes);
  out += ",\"used_bytes\":";
  out += std::to_string(used_bytes);
  out += ",\"free_bytes\":";
  out += std::to_string(free_bytes);
  out += ",\"max_bytes\":";
  out += std::to_string(CARD_IMAGE_MAX_BYTES);
  out += ",\"images\":[";
  bool first = true;
  CardImageHeader header {};
  if (partition != nullptr) for (size_t offset = 0; offset + sizeof(header) <= partition->size; offset += CARD_IMAGE_FLASH_SECTOR_SIZE) {
    if (!read_card_image_header_at(offset, header)) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"id\":";
    append_json_string(out, header.id);
    out += ",\"name\":";
    append_json_string(out, card_image_display_name(header).c_str());
    out += ",\"size\":";
    out += std::to_string(header.size);
    out += ",\"url\":\"/card-images/";
    out += header.id;
    out += ".jpg\"}";
  }
  out += "]}";
  return out;
}

size_t card_image_count() {
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) return 0;
  size_t count = 0;
  CardImageHeader header {};
  for (size_t offset = 0; offset + sizeof(header) <= partition->size; offset += CARD_IMAGE_FLASH_SECTOR_SIZE) {
    if (read_card_image_header_at(offset, header)) count++;
  }
  return count;
}

std::string next_card_image_id() {
  uint32_t now = esphome::millis();
  for (int i = 0; i < 100; i++) {
    std::string id = "img-" + std::to_string(now) + "-" + std::to_string(i);
    if (find_card_image_offset(id) < 0) return id;
  }
  return "";
}

bool handle_card_image_get(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_GET) return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url(request->url_to(url_buf));
  if (url == "/api/card-images") {
    std::string body = card_image_list_json();
    request->send(200, "application/json", body.c_str());
    return true;
  }
  if (url.rfind("/card-images/", 0) != 0) return false;
  std::string id = card_image_id_from_url(url, "/card-images/");
  if (id.empty()) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  int image_offset = find_card_image_offset(id);
  if (image_offset < 0) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  CardImageHeader header {};
  if (!read_card_image_header_at(static_cast<size_t>(image_offset), header)) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  const esp_partition_t *partition = card_image_partition();
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_set_type(req, "image/jpeg");
  // IDs are compact and can be reused after a reboot, so stale bytes must not
  // survive deletion in a browser cache.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  std::unique_ptr<char[]> buffer(new char[1024]);
  size_t remaining = header.size;
  size_t offset = 0;
  while (remaining > 0) {
    size_t chunk = remaining > 1024 ? 1024 : remaining;
    if (esp_partition_read(partition, static_cast<size_t>(image_offset) + sizeof(CardImageHeader) + offset,
                           buffer.get(), chunk) != ESP_OK) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return true;
    }
    if (httpd_resp_send_chunk(req, buffer.get(), chunk) != ESP_OK) {
      return true;
    }
    offset += chunk;
    remaining -= chunk;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return true;
}

bool handle_card_image_delete(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_DELETE) return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url(request->url_to(url_buf));
  if (url.rfind("/api/card-images/", 0) != 0) return false;
  std::string id = card_image_id_from_url(url, "/api/card-images/");
  if (id.empty()) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  int image_offset = find_card_image_offset(id);
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) {
    request->send(503, "text/plain",
                  "Card image storage is unavailable. Reflash this device over USB once to install it.");
    return true;
  }
  if (image_offset < 0) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  CardImageHeader header {};
  if (!read_card_image_header_at(static_cast<size_t>(image_offset), header)) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  esp_err_t err = esp_partition_erase_range(
    partition, static_cast<size_t>(image_offset), card_image_record_size(header.size));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to delete card image: %s", esp_err_to_name(err));
    request->send(500, "text/plain", "Delete failed");
    return true;
  }
  request->send(200, "application/json", "{\"ok\":true}");
  return true;
}

bool handle_card_image_rename(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_POST) return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url(request->url_to(url_buf));
  static constexpr const char *prefix = "/api/card-images/";
  static constexpr const char *suffix = "/rename";
  if (url.rfind(prefix, 0) != 0 || url.size() <= strlen(prefix) + strlen(suffix) ||
      url.compare(url.size() - strlen(suffix), strlen(suffix), suffix) != 0) {
    return false;
  }
  std::string id = url.substr(strlen(prefix), url.size() - strlen(prefix) - strlen(suffix));
  if (!card_image_id_valid(id)) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  std::string name = normalize_card_image_name(request->arg("name"));
  CardImageHeader header {};
  esp_err_t err = update_card_image_name(id, name, header);
  if (err == ESP_ERR_NOT_FOUND) {
    request->send(404, "text/plain", "Not found");
    return true;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to update card image name: %s", esp_err_to_name(err));
    request->send(500, "text/plain", "Rename failed");
    return true;
  }
  std::string body = card_image_item_json(header);
  request->send(200, "application/json", body.c_str());
  return true;
}

bool is_card_image_shortcut_request(AsyncWebServerRequest *request) {
  if (request->method() != HTTP_GET && request->method() != HTTP_DELETE) return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url(request->url_to(url_buf));
  if (request->method() == HTTP_GET) {
    return url == "/api/card-images" || url.rfind("/api/card-images/", 0) == 0 || url.rfind("/card-images/", 0) == 0;
  }
  if (request->method() == HTTP_POST) {
    return url.rfind("/api/card-images/", 0) == 0;
  }
  return url.rfind("/api/card-images/", 0) == 0;
}

esp_err_t handle_card_image_upload(httpd_req_t *r) {
  if (strcmp(r->uri, "/api/card-images") != 0) return ESP_ERR_NOT_FOUND;
  auto content_type = request_get_header(r, "Content-Type");
  if (!content_type.has_value() || strcasestr_n(content_type.value().c_str(), content_type.value().size(), "image/jpeg") == nullptr) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "JPEG required");
    return ESP_OK;
  }
  if (r->content_len == 0 || r->content_len > CARD_IMAGE_MAX_BYTES) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Image too large");
    return ESP_OK;
  }
  const esp_partition_t *partition = card_image_partition();
  if (partition == nullptr) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Card image storage is unavailable. Reflash this device over USB once to install the image storage partition.");
    return ESP_OK;
  }
  std::string id = next_card_image_id();
  if (id.empty()) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not allocate image id");
    return ESP_OK;
  }
  int image_offset = find_empty_card_image_offset(r->content_len);
  size_t record_size = card_image_record_size(r->content_len);
  if (image_offset < 0) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Not enough image storage space");
    return ESP_OK;
  }
  std::unique_ptr<char[]> buffer(new char[1024]);
  size_t record_offset = static_cast<size_t>(image_offset);
  esp_err_t err = esp_partition_erase_range(partition, record_offset, record_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase card image space: %s", esp_err_to_name(err));
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    return ESP_OK;
  }
  size_t remaining = r->content_len;
  size_t offset = 0;
  while (remaining > 0) {
    size_t want = remaining > 1024 ? 1024 : remaining;
    int ret = httpd_req_recv(r, buffer.get(), want);
    if (ret <= 0) {
      esp_partition_erase_range(partition, record_offset, record_size);
      httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Upload failed");
      return ESP_OK;
    }
    err = esp_partition_write(partition, record_offset + sizeof(CardImageHeader) + offset,
                              buffer.get(), static_cast<size_t>(ret));
    if (err != ESP_OK) {
      esp_partition_erase_range(partition, record_offset, record_size);
      ESP_LOGE(TAG, "Failed to write card image chunk: %s", esp_err_to_name(err));
      httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
      return ESP_OK;
    }
    offset += static_cast<size_t>(ret);
    remaining -= static_cast<size_t>(ret);
  }
  CardImageHeader header {};
  header.magic = CARD_IMAGE_MAGIC;
  header.version = CARD_IMAGE_VERSION;
  header.size = r->content_len;
  strlcpy(header.id, id.c_str(), sizeof(header.id));
  strlcpy(header.name, id.c_str(), sizeof(header.name));
  err = esp_partition_write(partition, record_offset, &header, sizeof(header));
  if (err != ESP_OK) {
    esp_partition_erase_range(partition, record_offset, record_size);
    ESP_LOGE(TAG, "Failed to write card image header: %s", esp_err_to_name(err));
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    return ESP_OK;
  }
  std::string body = "{\"id\":";
  append_json_string(body, id.c_str());
  body += ",\"name\":";
  append_json_string(body, card_image_display_name(header).c_str());
  body += ",\"size\":";
  body += std::to_string(r->content_len);
  body += ",\"url\":\"/card-images/";
  body += id;
  body += ".jpg\"}";
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr(r, body.c_str());
  return ESP_OK;
}

esp_err_t handle_card_image_rename_post(httpd_req_t *r) {
  static constexpr const char *prefix = "/api/card-images/";
  static constexpr const char *suffix = "/rename";
  std::string url(r->uri);
  const char *query = strchr(url.c_str(), '?');
  if (query != nullptr) url.resize(static_cast<size_t>(query - url.c_str()));
  if (url.rfind(prefix, 0) != 0 || url.size() <= strlen(prefix) + strlen(suffix) ||
      url.compare(url.size() - strlen(suffix), strlen(suffix), suffix) != 0) {
    return ESP_ERR_NOT_FOUND;
  }
  std::string id = url.substr(strlen(prefix), url.size() - strlen(prefix) - strlen(suffix));
  if (!card_image_id_valid(id)) {
    httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
  }
  if (r->content_len > 256) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Name too long");
    return ESP_OK;
  }
  std::string post_query;
  if (r->content_len > 0) {
    post_query.resize(r->content_len);
    size_t received = 0;
    while (received < r->content_len) {
      int ret = httpd_req_recv(r, &post_query[received], r->content_len - received);
      if (ret <= 0) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Rename failed");
        return ESP_OK;
      }
      received += static_cast<size_t>(ret);
    }
  }
  auto parsed_name = query_key_value(post_query.c_str(), post_query.size(), "name");
  std::string name = normalize_card_image_name(parsed_name.has_value() ? parsed_name.value() : "");
  CardImageHeader header {};
  esp_err_t err = update_card_image_name(id, name, header);
  if (err == ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to update card image name: %s", esp_err_to_name(err));
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
    return ESP_OK;
  }
  std::string body = card_image_item_json(header);
  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr(r, body.c_str());
  return ESP_OK;
}

// Non-blocking send function to prevent watchdog timeouts when TCP buffers are full
/**
 * Sends data on a socket in non-blocking mode.
 *
 * @param hd      HTTP server handle (unused).
 * @param sockfd  Socket file descriptor.
 * @param buf     Buffer to send.
 * @param buf_len Length of buffer.
 * @param flags   Flags for send().
 * @return
 *   - Number of bytes sent on success.
 *   - HTTPD_SOCK_ERR_INVALID if buf is nullptr.
 *   - HTTPD_SOCK_ERR_TIMEOUT if the send buffer is full (EAGAIN/EWOULDBLOCK).
 *   - HTTPD_SOCK_ERR_FAIL for other errors.
 */
int nonblocking_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags) {
  if (buf == nullptr) {
    return HTTPD_SOCK_ERR_INVALID;
  }

  // Use MSG_DONTWAIT to prevent blocking when TCP send buffer is full
  int ret = send(sockfd, buf, buf_len, flags | MSG_DONTWAIT);
  if (ret < 0) {
    const int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) {
      // Buffer full - retry later
      return HTTPD_SOCK_ERR_TIMEOUT;
    }
    // Real error
    ESP_LOGD(TAG, "send error: errno %d", err);
    return HTTPD_SOCK_ERR_FAIL;
  }
  return ret;
}
}  // namespace

void AsyncWebServer::safe_close_with_shutdown(httpd_handle_t hd, int sockfd) {
  // CRITICAL: Shut down receive BEFORE closing to prevent lwIP race conditions
  //
  // The race condition occurs because close() initiates lwIP teardown while
  // the TCP/IP thread can still receive packets, causing assertions when
  // recv_tcp() sees partially-torn-down state.
  //
  // By shutting down receive first, we tell lwIP to stop accepting new data BEFORE
  // the teardown begins, eliminating the race window. We only shutdown RD (not RDWR)
  // to allow the FIN packet to be sent cleanly during close().
  //
  // Note: This function may be called with an already-closed socket if the network
  // stack closed it. In that case, shutdown() will fail but close() is safe to call.
  //
  // See: https://github.com/esphome/esphome-webserver/issues/163

  // Attempt shutdown - ignore errors as socket may already be closed
  shutdown(sockfd, SHUT_RD);

  // Always close - safe even if socket is already closed by network stack
  close(sockfd);
}

void AsyncWebServer::end() {
  if (this->server_) {
    httpd_stop(this->server_);
    this->server_ = nullptr;
  }
}

void AsyncWebServer::begin() {
  if (this->server_) {
    this->end();
  }
  // The ESPControl web UI exposes many internal configuration entities. Larger
  // P4 panels can overflow the ESP-IDF default while serving entity details.
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 16384;
  // Keep browser bursts from opening several web sessions at once. The config
  // UI fetches details sequentially, so two client sockets are enough and leave
  // more internal heap available for LVGL/display work on P4 panels.
  config.max_open_sockets = 5;
  config.backlog_conn = 2;
  config.server_port = this->port_;
  config.uri_match_fn = [](const char * /*unused*/, const char * /*unused*/, size_t /*unused*/) { return true; };
  // Always enable LRU purging to handle socket exhaustion gracefully.
  // When max sockets is reached, the oldest connection is closed to make room for new ones.
  // This prevents "httpd_accept_conn: error in accept (23)" errors.
  // See: https://github.com/esphome/esphome/issues/12464
  config.lru_purge_enable = true;
  // Use custom close function that shuts down before closing to prevent lwIP race conditions
  config.close_fn = AsyncWebServer::safe_close_with_shutdown;
  if (httpd_start(&this->server_, &config) == ESP_OK) {
    global_async_web_server() = this;
    const httpd_uri_t handler_get = {
        .uri = "",
        .method = HTTP_GET,
        .handler = AsyncWebServer::request_handler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(this->server_, &handler_get);

    const httpd_uri_t handler_post = {
        .uri = "",
        .method = HTTP_POST,
        .handler = AsyncWebServer::request_post_handler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(this->server_, &handler_post);

    const httpd_uri_t handler_options = {
        .uri = "",
        .method = HTTP_OPTIONS,
        .handler = AsyncWebServer::request_handler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(this->server_, &handler_options);

    const httpd_uri_t handler_delete = {
        .uri = "",
        .method = HTTP_DELETE,
        .handler = AsyncWebServer::request_handler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(this->server_, &handler_delete);
  }
}

esp_err_t AsyncWebServer::request_post_handler(httpd_req_t *r) {
  ESP_LOGVV(TAG, "Enter AsyncWebServer::request_post_handler. uri=%s", r->uri);
  if (strcmp(r->uri, "/api/card-images") == 0) {
#ifdef USE_WEBSERVER_AUTH
    AsyncWebServerRequest req(r);
    auto *server = static_cast<AsyncWebServer *>(r->user_ctx);
    if (!server->authenticate_shortcut_request_(&req)) return ESP_OK;
#endif
    return handle_card_image_upload(r);
  }
  if (strncmp(r->uri, "/api/card-images/", strlen("/api/card-images/")) == 0) {
#ifdef USE_WEBSERVER_AUTH
    AsyncWebServerRequest req(r);
    auto *server = static_cast<AsyncWebServer *>(r->user_ctx);
    if (!server->authenticate_shortcut_request_(&req)) return ESP_OK;
#endif
    esp_err_t card_image_result = handle_card_image_rename_post(r);
    if (card_image_result != ESP_ERR_NOT_FOUND) return card_image_result;
  }
  auto content_type = request_get_header(r, "Content-Type");

  if (!request_has_header(r, "Content-Length")) {
    ESP_LOGW(TAG, "Content length is required for post: %s", r->uri);
    httpd_resp_send_err(r, HTTPD_411_LENGTH_REQUIRED, nullptr);
    return ESP_OK;
  }

  if (content_type.has_value()) {
    const char *content_type_char = content_type.value().c_str();

    // Check most common case first
    size_t content_type_len = strlen(content_type_char);
    if (strcasestr_n(content_type_char, content_type_len, "application/x-www-form-urlencoded") != nullptr) {
      // Normal form data - proceed with regular handling
#ifdef USE_WEBSERVER_OTA
    } else if (strcasestr_n(content_type_char, content_type_len, "multipart/form-data") != nullptr) {
      auto *server = static_cast<AsyncWebServer *>(r->user_ctx);
      return server->handle_multipart_upload_(r, content_type_char);
#endif
    } else {
      ESP_LOGW(TAG, "Unsupported content type for POST: %s", content_type_char);
      // fallback to get handler to support backward compatibility
      return AsyncWebServer::request_handler(r);
    }
  }

  // Handle regular form data
  if (r->content_len > CONFIG_HTTPD_MAX_REQ_HDR_LEN) {
    ESP_LOGW(TAG, "Request size is to big: %zu", r->content_len);
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, nullptr);
    return ESP_FAIL;
  }

  std::string post_query;
  if (r->content_len > 0) {
    post_query.resize(r->content_len);
    size_t received = 0;
    while (received < r->content_len) {
      const int ret = httpd_req_recv(r, &post_query[received], r->content_len - received);
      if (ret <= 0) {  // 0 return value indicates connection closed
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
          httpd_resp_send_err(r, HTTPD_408_REQ_TIMEOUT, nullptr);
          return ESP_ERR_TIMEOUT;
        }
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, nullptr);
        return ESP_FAIL;
      }
      received += static_cast<size_t>(ret);
    }
    if (received != r->content_len) {
      ESP_LOGW(TAG, "Incomplete POST body for %s: got %zu of %zu bytes", r->uri, received, r->content_len);
      httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, nullptr);
      return ESP_FAIL;
    }
  }

  AsyncWebServerRequest req(r, std::move(post_query));
  return static_cast<AsyncWebServer *>(r->user_ctx)->request_handler_(&req);
}

esp_err_t AsyncWebServer::request_handler(httpd_req_t *r) {
  ESP_LOGVV(TAG, "Enter AsyncWebServer::request_handler. method=%u, uri=%s", r->method, r->uri);
  AsyncWebServerRequest req(r);
  return static_cast<AsyncWebServer *>(r->user_ctx)->request_handler_(&req);
}

esp_err_t AsyncWebServer::request_handler_(AsyncWebServerRequest *request) const {
  if (is_card_image_shortcut_request(request) && !is_loopback_request(*request)) {
#ifdef USE_WEBSERVER_AUTH
    if (!this->authenticate_shortcut_request_(request)) return ESP_OK;
#endif
  }
  if (handle_card_image_get(request) || handle_card_image_delete(request) || handle_card_image_rename(request)) {
    return ESP_OK;
  }
  if (handle_firmware_version_request(request)) {
    return ESP_OK;
  }
  for (auto *handler : this->handlers_) {
    if (handler->canHandle(request)) {
      // At now process only basic requests.
      // OTA requires multipart request support and handleUpload for it
      handler->handleRequest(request);
      return ESP_OK;
    }
  }
  if (this->on_not_found_) {
    this->on_not_found_(request);
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

#ifdef USE_WEBSERVER_AUTH
bool AsyncWebServer::authenticate_shortcut_request_(AsyncWebServerRequest *request) const {
  bool saw_handler = false;
  for (auto *handler : this->handlers_) {
    saw_handler = true;
    if (!handler->check_auth(request)) return false;
  }
  if (!saw_handler) {
    request->requestAuthentication();
    return false;
  }
  return true;
}
#endif

AsyncWebServerRequest::~AsyncWebServerRequest() {
  delete this->rsp_;
  for (auto *param : this->params_) {
    delete param;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

bool AsyncWebServerRequest::hasHeader(const char *name) const { return request_has_header(*this, name); }

optional<std::string> AsyncWebServerRequest::get_header(const char *name) const {
  return request_get_header(*this, name);
}

StringRef AsyncWebServerRequest::url_to(std::span<char, URL_BUF_SIZE> buffer) const {
  const char *uri = this->req_->uri;
  const char *query_start = strchr(uri, '?');
  size_t uri_len = query_start ? static_cast<size_t>(query_start - uri) : strlen(uri);
  size_t copy_len = std::min(uri_len, URL_BUF_SIZE - 1);
  memcpy(buffer.data(), uri, copy_len);
  buffer[copy_len] = '\0';
  // Decode URL-encoded characters in-place (e.g., %20 -> space)
  size_t decoded_len = url_decode(buffer.data());
  return StringRef(buffer.data(), decoded_len);
}

void AsyncWebServerRequest::redirect(const std::string &url) {
  httpd_resp_set_status(*this, "302 Found");
  httpd_resp_set_hdr(*this, "Location", url.c_str());
  httpd_resp_set_hdr(*this, "Connection", "close");
  httpd_resp_send(*this, nullptr, 0);
}

void AsyncWebServerRequest::init_response_(AsyncWebServerResponse *rsp, int code, const char *content_type) {
  // Set status code - use constants for common codes, default to 500 for unknown codes
  const char *status;
  switch (code) {
    case 200:
      status = HTTPD_200;
      break;
    case 404:
      status = HTTPD_404;
      break;
    case 409:
      status = HTTPD_409;
      break;
    default:
      status = HTTPD_500;
      break;
  }
  httpd_resp_set_status(*this, status);

  if (content_type && *content_type) {
    httpd_resp_set_type(*this, content_type);
  }
  httpd_resp_set_hdr(*this, "Accept-Ranges", "none");
  apply_no_cache_headers(*this);

  for (const auto &header : DefaultHeaders::Instance().headers_) {
    httpd_resp_set_hdr(*this, header.name, header.value);
  }

  delete this->rsp_;
  this->rsp_ = rsp;
}

#ifdef USE_WEBSERVER_AUTH
bool AsyncWebServerRequest::authenticate(const char *username, const char *password) const {
  if (username == nullptr || password == nullptr || *username == 0) {
    return true;
  }
  auto auth = this->get_header("Authorization");
  if (!auth.has_value()) {
    return false;
  }

  auto *auth_str = auth.value().c_str();

  const auto auth_prefix_len = sizeof("Basic ") - 1;
  if (strncmp("Basic ", auth_str, auth_prefix_len) != 0) {
    ESP_LOGW(TAG, "Only Basic authorization supported yet");
    return false;
  }

  // Build user:pass in stack buffer to avoid heap allocation
  constexpr size_t max_user_info_len = 256;
  char user_info[max_user_info_len];
  size_t user_len = strlen(username);
  size_t pass_len = strlen(password);
  size_t user_info_len = user_len + 1 + pass_len;

  if (user_info_len >= max_user_info_len) {
    ESP_LOGW(TAG, "Credentials too long for authentication");
    return false;
  }

  memcpy(user_info, username, user_len);
  user_info[user_len] = ':';
  memcpy(user_info + user_len + 1, password, pass_len);
  user_info[user_info_len] = '\0';

  // Base64 output size is ceil(input_len * 4/3) + 1, with input bounded to 256 bytes
  // max output is ceil(256 * 4/3) + 1 = 343 bytes, use 350 for safety
  constexpr size_t max_digest_len = 350;
  char digest[max_digest_len];
  size_t out;
  esp_crypto_base64_encode(reinterpret_cast<uint8_t *>(digest), max_digest_len, &out,
                           reinterpret_cast<const uint8_t *>(user_info), user_info_len);

  // Constant-time comparison to avoid timing side channels.
  // No early return on length mismatch — the length difference is folded
  // into the accumulator so any mismatch is rejected.
  const char *provided = auth_str + auth_prefix_len;
  size_t digest_len = out;  // length from esp_crypto_base64_encode
  // Derive provided_len from the already-sized std::string rather than
  // rescanning with strlen (avoids attacker-controlled scan length).
  size_t provided_len = auth.value().size() - auth_prefix_len;
  // Use full-width XOR so any bit difference in the lengths is preserved
  // (uint8_t truncation would miss differences in higher bytes, e.g.
  // digest_len vs digest_len + 256).
  volatile size_t result = digest_len ^ provided_len;
  // Iterate over the expected digest length only — the full-width length
  // XOR above already rejects any length mismatch, and bounding the loop
  // prevents a long Authorization header from forcing extra work.
  for (size_t i = 0; i < digest_len; i++) {
    char provided_ch = (i < provided_len) ? provided[i] : 0;
    result |= static_cast<uint8_t>(digest[i] ^ provided_ch);
  }
  return result == 0;
}

void AsyncWebServerRequest::requestAuthentication(const char *realm) const {
  httpd_resp_set_hdr(*this, "Connection", "keep-alive");
  // Note: realm is never configured in ESPHome, always nullptr -> "Login Required"
  (void) realm;  // Unused - always use default
  httpd_resp_set_hdr(*this, "WWW-Authenticate", "Basic realm=\"Login Required\"");
  httpd_resp_send_err(*this, HTTPD_401_UNAUTHORIZED, nullptr);
}
#endif

AsyncWebParameter *AsyncWebServerRequest::getParam(const char *name) {
  // Check cache first - only successful lookups are cached
  for (auto *param : this->params_) {
    if (param->name() == name) {
      return param;
    }
  }

  // Look up value from query strings
  auto val = this->find_query_value_(name);

  // Don't cache misses to avoid wasting memory when handlers check for
  // optional parameters that don't exist in the request
  if (!val.has_value()) {
    return nullptr;
  }

  auto *param = new AsyncWebParameter(name, val.value());  // NOLINT(cppcoreguidelines-owning-memory)
  this->params_.push_back(param);
  return param;
}

/// Search post_query then URL query with a callback.
/// Returns first truthy result, or value-initialized default.
/// URL query is accessed directly from req->uri (same pattern as url_to()).
template<typename Func>
static auto search_query_sources(httpd_req_t *req, const std::string &post_query, const char *name, Func func)
    -> decltype(func(nullptr, size_t{0}, name)) {
  if (!post_query.empty()) {
    auto result = func(post_query.c_str(), post_query.size(), name);
    if (result) {
      return result;
    }
  }
  // Use httpd API for query length, then access string directly from URI.
  // http_parser identifies components by offset/length without modifying the URI string.
  // This is the same pattern used by url_to().
  auto len = httpd_req_get_url_query_len(req);
  if (len == 0) {
    return {};
  }
  const char *query = strchr(req->uri, '?');
  if (query == nullptr) {
    return {};
  }
  query++;  // skip '?'
  return func(query, len, name);
}

optional<std::string> AsyncWebServerRequest::find_query_value_(const char *name) const {
  return search_query_sources(this->req_, this->post_query_, name,
                              [](const char *q, size_t len, const char *k) { return query_key_value(q, len, k); });
}

bool AsyncWebServerRequest::hasArg(const char *name) {
  return search_query_sources(this->req_, this->post_query_, name, query_has_key);
}

std::string AsyncWebServerRequest::arg(const char *name) {
  auto val = this->find_query_value_(name);
  if (val.has_value()) {
    return std::move(val.value());
  }
  return {};
}

void AsyncWebServerResponse::addHeader(const char *name, const char *value) {
  httpd_resp_set_hdr(*this->req_, name, value);
}

void AsyncResponseStream::print(float value) {
  // Use stack buffer to avoid temporary string allocation
  // Size: sign (1) + digits (10) + decimal (1) + precision (6) + exponent (5) + null (1) = 24, use 32 for safety
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%f", value);
  this->content_.append(buf, len);
}

void AsyncResponseStream::printf(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  const int length = vsnprintf(nullptr, 0, fmt, args);
  va_end(args);
  if (length < 0) {
    return;
  }

  std::string str;
  str.resize(length + 1);

  va_start(args, fmt);
  vsnprintf(str.data(), str.size(), fmt, args);
  va_end(args);
  str.resize(length);

  this->print(str);
}

#ifdef USE_WEBSERVER
AsyncEventSource::~AsyncEventSource() {
  for (auto *ses : this->sessions_) {
    delete ses;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

void AsyncEventSource::handleRequest(AsyncWebServerRequest *request) {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,clang-analyzer-cplusplus.NewDeleteLeaks)
  auto *rsp = new AsyncEventSourceResponse(request, this, this->web_server_);
  if (this->on_connect_) {
    this->on_connect_(rsp);
  }
  this->sessions_.push_back(rsp);
  // Wake up WebServer::loop() to drain deferred event queues for this client.
  // Safe from httpd task context via the pending_enable_loop_ flag.
  this->web_server_->enable_loop_soon_any_context();
}

bool AsyncEventSource::loop() {
  // Clean up dead sessions safely
  // This follows the ESP-IDF pattern where free_ctx marks resources as dead
  // and the main loop handles the actual cleanup to avoid race conditions
  for (size_t i = 0; i < this->sessions_.size();) {
    auto *ses = this->sessions_[i];
    // If the session has a dead socket (marked by destroy callback)
    if (ses->fd_.load() == 0) {
      ESP_LOGD(TAG, "Removing dead event source session");
      delete ses;  // NOLINT(cppcoreguidelines-owning-memory)
      // Remove by swapping with last element (O(1) removal, order doesn't matter for sessions)
      this->sessions_[i] = this->sessions_.back();
      this->sessions_.pop_back();
    } else {
      ses->loop();
      ++i;
    }
  }
  return !this->sessions_.empty();
}

void AsyncEventSource::try_send_nodefer(const char *message, const char *event, uint32_t id, uint32_t reconnect) {
  for (auto *ses : this->sessions_) {
    if (ses->fd_.load() != 0) {  // Skip dead sessions
      ses->try_send_nodefer(message, event, id, reconnect);
    }
  }
}

void AsyncEventSource::deferrable_send_state(void *source, const char *event_type,
                                             message_generator_t *message_generator) {
  // Skip if no connected clients to avoid unnecessary processing
  if (this->empty())
    return;
  for (auto *ses : this->sessions_) {
    if (ses->fd_.load() != 0) {  // Skip dead sessions
      ses->deferrable_send_state(source, event_type, message_generator);
    }
  }
}

AsyncEventSourceResponse::AsyncEventSourceResponse(const AsyncWebServerRequest *request,
                                                   esphome::web_server_idf::AsyncEventSource *server,
                                                   esphome::web_server::WebServer *ws)
    : server_(server), web_server_(ws), entities_iterator_(ws, server) {
  httpd_req_t *req = *request;

  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");

  for (const auto &header : DefaultHeaders::Instance().headers_) {
    httpd_resp_set_hdr(req, header.name, header.value);
  }

  httpd_resp_send_chunk(req, CRLF_STR, CRLF_LEN);

  req->sess_ctx = this;
  req->free_ctx = AsyncEventSourceResponse::destroy;

  this->hd_ = req->handle;
  this->fd_.store(httpd_req_to_sockfd(req));

  // Use non-blocking send to prevent watchdog timeouts when TCP buffers are full
  httpd_sess_set_send_override(this->hd_, this->fd_.load(), nonblocking_send);

  // Configure reconnect timeout and send config
  // this should always go through since the tcp send buffer is empty on connect
  auto message = ws->get_config_json();
  this->try_send_nodefer(message.c_str(), "ping", millis(), 30000);

#ifdef USE_WEBSERVER_SORTING
  for (auto &group : ws->sorting_groups_) {
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) false positive with ArduinoJson
    json::JsonBuilder builder;
    JsonObject root = builder.root();
    root["name"] = group.second.name;
    root["sorting_weight"] = group.second.weight;
    message = builder.serialize();
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

    // a (very) large number of these should be able to be queued initially without defer
    // since the only thing in the send buffer at this point is the initial ping/config
    this->try_send_nodefer(message.c_str(), "sorting_group");
  }
#endif

  this->entities_iterator_.begin(ws->include_internal_);

  // just dump them all up-front and take advantage of the deferred queue
  //     on second thought that takes too long, but leaving the commented code here for debug purposes
  // while(!this->entities_iterator_.completed()) {
  //  this->entities_iterator_.advance();
  //}
}

void AsyncEventSourceResponse::destroy(void *ptr) {
  auto *rsp = static_cast<AsyncEventSourceResponse *>(ptr);
  int fd = rsp->fd_.exchange(0);  // Atomically get and clear fd
  ESP_LOGD(TAG, "Event source connection closed (fd: %d)", fd);
  // Mark as dead - will be cleaned up in the main loop
  // Note: We don't delete or remove from set here to avoid race conditions
  // httpd will call our custom close_fn (safe_close_with_shutdown) which handles
  // shutdown() before close() to prevent lwIP race conditions
}

// helper for allowing only unique entries in the queue
void AsyncEventSourceResponse::deq_push_back_with_dedup_(void *source, message_generator_t *message_generator) {
  DeferredEvent item(source, message_generator);

  // Use range-based for loop instead of std::find_if to reduce template instantiation overhead and binary size
  for (auto &event : this->deferred_queue_) {
    if (event == item) {
      return;  // Already in queue, no need to update since items are equal
    }
  }
  this->deferred_queue_.push_back(item);
}

void AsyncEventSourceResponse::process_deferred_queue_() {
  while (!deferred_queue_.empty()) {
    DeferredEvent &de = deferred_queue_.front();
    auto message = de.message_generator_(web_server_, de.source_);
    if (this->try_send_nodefer(message.c_str(), "state")) {
      // O(n) but memory efficiency is more important than speed here which is why std::vector was chosen
      deferred_queue_.erase(deferred_queue_.begin());
    } else {
      break;
    }
  }
}

void AsyncEventSourceResponse::process_buffer_() {
  if (event_buffer_.empty()) {
    return;
  }
  if (event_bytes_sent_ == event_buffer_.size()) {
    event_buffer_.resize(0);
    event_bytes_sent_ = 0;
    return;
  }

  size_t remaining = event_buffer_.size() - event_bytes_sent_;
  int bytes_sent =
      httpd_socket_send(this->hd_, this->fd_.load(), event_buffer_.c_str() + event_bytes_sent_, remaining, 0);
  if (bytes_sent == HTTPD_SOCK_ERR_TIMEOUT) {
    // EAGAIN/EWOULDBLOCK - socket buffer full, try again later
    // NOTE: Similar logic exists in web_server/web_server.cpp in DeferredUpdateEventSource::process_deferred_queue_()
    // The implementations differ due to platform-specific APIs (HTTPD_SOCK_ERR_TIMEOUT vs DISCARDED, fd_.store(0) vs
    // close()), but the failure counting and timeout logic should be kept in sync. If you change this logic, also
    // update the Arduino implementation.
    this->consecutive_send_failures_++;
    if (this->consecutive_send_failures_ >= MAX_CONSECUTIVE_SEND_FAILURES) {
      // Too many failures, connection is likely dead
      ESP_LOGW(TAG, "Closing stuck EventSource connection after %" PRIu16 " failed sends",
               this->consecutive_send_failures_);
      this->fd_.store(0);  // Mark for cleanup
      this->deferred_queue_.clear();
    }
    return;
  }
  if (bytes_sent == HTTPD_SOCK_ERR_FAIL) {
    // Real socket error - connection will be closed by httpd and destroy callback will be called
    return;
  }
  if (bytes_sent <= 0) {
    // Unexpected error or zero bytes sent
    ESP_LOGW(TAG, "Unexpected send result: %d", bytes_sent);
    return;
  }

  // Successful send - reset failure counter
  this->consecutive_send_failures_ = 0;
  event_bytes_sent_ += bytes_sent;

  // Log partial sends for debugging
  if (event_bytes_sent_ < event_buffer_.size()) {
    ESP_LOGV(TAG, "Partial send: %d/%zu bytes (total: %zu/%zu)", bytes_sent, remaining, event_bytes_sent_,
             event_buffer_.size());
  }

  if (event_bytes_sent_ == event_buffer_.size()) {
    event_buffer_.resize(0);
    event_bytes_sent_ = 0;
  }
}

void AsyncEventSourceResponse::loop() {
  process_buffer_();
  process_deferred_queue_();
  if (!this->entities_iterator_.completed())
    this->entities_iterator_.advance();
}

bool AsyncEventSourceResponse::try_send_nodefer(const char *message, const char *event, uint32_t id,
                                                uint32_t reconnect) {
  if (this->fd_.load() == 0) {
    return false;
  }

  process_buffer_();
  if (!event_buffer_.empty()) {
    // there is still pending event data to send first
    return false;
  }

  // 8 spaces are standing in for the hexidecimal chunk length to print later
  const char chunk_len_header[] = "        " CRLF_STR;
  const int chunk_len_header_len = sizeof(chunk_len_header) - 1;

  event_buffer_.append(chunk_len_header);

  // Use stack buffer for formatting numeric fields to avoid temporary string allocations
  // Size: "retry: " (7) + max uint32 (10 digits) + CRLF (2) + null (1) = 20 bytes, use 32 for safety
  constexpr size_t num_buf_size = 32;
  char num_buf[num_buf_size];

  if (reconnect) {
    int len = snprintf(num_buf, num_buf_size, "retry: %" PRIu32 CRLF_STR, reconnect);
    event_buffer_.append(num_buf, len);
  }

  if (id) {
    int len = snprintf(num_buf, num_buf_size, "id: %" PRIu32 CRLF_STR, id);
    event_buffer_.append(num_buf, len);
  }

  if (event && *event) {
    event_buffer_.append("event: ", sizeof("event: ") - 1);
    event_buffer_.append(event);
    event_buffer_.append(CRLF_STR, CRLF_LEN);
  }

  // Match ESPAsyncWebServer: null message means no data lines and no terminating blank line
  if (message) {
    // SSE spec requires each line of a multi-line message to have its own "data:" prefix
    // Handle \n, \r, and \r\n line endings (matching ESPAsyncWebServer behavior)

    // Fast path: check if message contains any newlines at all
    // Most SSE messages (JSON state updates) have no newlines
    const char *first_n = strchr(message, '\n');
    const char *first_r = strchr(message, '\r');

    if (first_n == nullptr && first_r == nullptr) {
      // No newlines - fast path (most common case)
      event_buffer_.append("data: ", sizeof("data: ") - 1);
      event_buffer_.append(message);
      event_buffer_.append(CRLF_STR CRLF_STR, CRLF_LEN * 2);  // data line + blank line terminator
    } else {
      // Has newlines - handle multi-line message
      const char *line_start = message;
      size_t msg_len = strlen(message);
      const char *msg_end = message + msg_len;

      // Reuse the first search results
      const char *next_n = first_n;
      const char *next_r = first_r;

      while (line_start <= msg_end) {
        const char *line_end;
        const char *next_line;

        if (next_n == nullptr && next_r == nullptr) {
          // No more line breaks - output remaining text as final line
          event_buffer_.append("data: ", sizeof("data: ") - 1);
          event_buffer_.append(line_start);
          event_buffer_.append(CRLF_STR, CRLF_LEN);
          break;
        }

        // Determine line ending type and next line start
        if (next_n != nullptr && next_r != nullptr) {
          if (next_r + 1 == next_n) {
            // \r\n sequence
            line_end = next_r;
            next_line = next_n + 1;
          } else {
            // Mixed \n and \r - use whichever comes first
            line_end = (next_r < next_n) ? next_r : next_n;
            next_line = line_end + 1;
          }
        } else if (next_n != nullptr) {
          // Unix LF
          line_end = next_n;
          next_line = next_n + 1;
        } else {
          // Old Mac CR
          line_end = next_r;
          next_line = next_r + 1;
        }

        // Output this line
        event_buffer_.append("data: ", sizeof("data: ") - 1);
        event_buffer_.append(line_start, line_end - line_start);
        event_buffer_.append(CRLF_STR, CRLF_LEN);

        line_start = next_line;

        // Check if we've consumed all content
        if (line_start >= msg_end) {
          break;
        }

        // Search for next newlines only in remaining string
        next_n = strchr(line_start, '\n');
        next_r = strchr(line_start, '\r');
      }

      // Terminate message with blank line
      event_buffer_.append(CRLF_STR, CRLF_LEN);
    }
  }

  if (event_buffer_.size() == static_cast<size_t>(chunk_len_header_len)) {
    // Nothing was added, reset buffer
    event_buffer_.resize(0);
    return true;
  }

  event_buffer_.append(CRLF_STR, CRLF_LEN);

  // chunk length header itself and the final chunk terminating CRLF are not counted as part of the chunk
  int chunk_len = event_buffer_.size() - CRLF_LEN - chunk_len_header_len;
  char chunk_len_str[9];
  snprintf(chunk_len_str, 9, "%08x", chunk_len);
  std::memcpy(&event_buffer_[0], chunk_len_str, 8);

  event_bytes_sent_ = 0;
  process_buffer_();

  return true;
}

void AsyncEventSourceResponse::deferrable_send_state(void *source, const char *event_type,
                                                     message_generator_t *message_generator) {
  // allow all json "details_all" to go through before publishing bare state events, this avoids unnamed entries showing
  // up in the web GUI and reduces event load during initial connect
  if (!this->entities_iterator_.completed() && 0 != strcmp(event_type, "state_detail_all"))
    return;

  if (source == nullptr)
    return;
  if (event_type == nullptr)
    return;
  if (message_generator == nullptr)
    return;

  if (0 != strcmp(event_type, "state_detail_all") && 0 != strcmp(event_type, "state")) {
    ESP_LOGE(TAG, "Can't defer non-state event");
  }

  process_buffer_();
  process_deferred_queue_();

  if (!event_buffer_.empty() || !deferred_queue_.empty()) {
    // outgoing event buffer or deferred queue still not empty which means downstream tcp send buffer full, no point
    // trying to send first
    deq_push_back_with_dedup_(source, message_generator);
  } else {
    auto message = message_generator(web_server_, source);
    if (!this->try_send_nodefer(message.c_str(), "state")) {
      deq_push_back_with_dedup_(source, message_generator);
    }
  }
}
#endif

#ifdef USE_WEBSERVER_OTA
esp_err_t AsyncWebServer::handle_multipart_upload_(httpd_req_t *r, const char *content_type) {
  static constexpr size_t MULTIPART_CHUNK_SIZE = 1460;       // Match Arduino AsyncWebServer buffer size
  static constexpr size_t YIELD_INTERVAL_BYTES = 16 * 1024;  // Yield every 16KB to prevent watchdog

  // Parse boundary and create reader
  const char *boundary_start;
  size_t boundary_len;
  if (!parse_multipart_boundary(content_type, &boundary_start, &boundary_len)) {
    ESP_LOGE(TAG, "Failed to parse multipart boundary");
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, nullptr);
    return ESP_FAIL;
  }

  AsyncWebServerRequest req(r);
  AsyncWebHandler *handler = nullptr;
  for (auto *h : this->handlers_) {
    if (h->canHandle(&req)) {
      handler = h;
      break;
    }
  }

  if (!handler) {
    ESP_LOGW(TAG, "No handler found for OTA request");
    httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, nullptr);
    return ESP_OK;
  }

  // Upload state
  std::string filename;
  size_t index = 0;
  // Create reader on heap to reduce stack usage
  auto reader = std::make_unique<MultipartReader>("--" + std::string(boundary_start, boundary_len));

  // Configure callbacks
  reader->set_data_callback([&](const uint8_t *data, size_t len) {
    if (!reader->has_file() || !len)
      return;

    if (filename.empty()) {
      filename = reader->get_current_part().filename;
      ESP_LOGV(TAG, "Processing file: '%s'", filename.c_str());
      handler->handleUpload(&req, filename, 0, nullptr, 0, false);  // Start
    }

    handler->handleUpload(&req, filename, index, const_cast<uint8_t *>(data), len, false);
    index += len;
  });

  reader->set_part_complete_callback([&]() {
    if (index > 0) {
      handler->handleUpload(&req, filename, index, nullptr, 0, true);  // End
      filename.clear();
      index = 0;
    }
  });

  // Use heap buffer - 1460 bytes is too large for the httpd task stack
  auto buffer = std::make_unique_for_overwrite<char[]>(MULTIPART_CHUNK_SIZE);
  size_t bytes_since_yield = 0;

  for (size_t remaining = r->content_len; remaining > 0;) {
    int recv_len = httpd_req_recv(r, buffer.get(), std::min(remaining, MULTIPART_CHUNK_SIZE));

    if (recv_len <= 0) {
      httpd_resp_send_err(r, recv_len == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT : HTTPD_400_BAD_REQUEST,
                          nullptr);
      return recv_len == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    if (reader->parse(buffer.get(), recv_len) != static_cast<size_t>(recv_len)) {
      ESP_LOGW(TAG, "Multipart parser error");
      httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, nullptr);
      return ESP_FAIL;
    }

    remaining -= recv_len;
    bytes_since_yield += recv_len;

    if (bytes_since_yield > YIELD_INTERVAL_BYTES) {
      vTaskDelay(1);
      bytes_since_yield = 0;
    }
  }

  handler->handleRequest(&req);
  return ESP_OK;
}
#endif  // USE_WEBSERVER_OTA

}  // namespace esphome::web_server_idf

#endif  // !defined(USE_ESP32)
