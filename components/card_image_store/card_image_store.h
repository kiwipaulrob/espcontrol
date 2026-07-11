#pragma once

#include "esphome/components/http_request/http_request.h"

#ifdef USE_ESP32
#include "esp_partition.h"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome::card_image_store {

static constexpr size_t CARD_IMAGE_MAX_BYTES = 64 * 1024;
static constexpr size_t CARD_IMAGE_NAME_MAX_LENGTH = 40;
static constexpr size_t CARD_IMAGE_FLASH_SECTOR_SIZE = 4096;
static constexpr uint32_t CARD_IMAGE_FORMAT_VERSION = 2;

struct CardImageInfo {
  std::string id;
  std::string name;
  size_t size{0};
  size_t offset{0};
  uint32_t crc32{0};
};

struct CardImageUpload {
  std::string id;
  size_t offset{0};
  size_t size{0};
  size_t written{0};
  size_t record_size{0};
  uint32_t crc32{0xFFFFFFFFu};
  uint8_t first_bytes[2]{0, 0};
  uint8_t last_bytes[2]{0, 0};
};

class CardImageStore;

class CardImageReader : public http_request::HttpContainer {
 public:
  CardImageReader(CardImageStore *store, const CardImageInfo &info);
  ~CardImageReader() override { this->end(); }

  int read(uint8_t *buf, size_t max_len) override;
  void end() override;

 protected:
  CardImageStore *store_{nullptr};
  CardImageInfo info_{};
  size_t position_{0};
  bool ended_{false};
};

class CardImageStore {
 public:
  static CardImageStore &instance();

  bool available();
  size_t capacity();
  size_t used_bytes();
  size_t free_bytes();
  const std::vector<CardImageInfo> &list();
  bool find(const std::string &id, CardImageInfo &out);

  esp_err_t begin_upload(size_t size, CardImageUpload &upload);
  esp_err_t write_upload(CardImageUpload &upload, const uint8_t *data, size_t size);
  esp_err_t commit_upload(CardImageUpload &upload, CardImageInfo &out);
  void abort_upload(CardImageUpload &upload);
  esp_err_t rename(const std::string &id, const std::string &name, CardImageInfo &out);
  esp_err_t erase(const std::string &id);

  std::shared_ptr<http_request::HttpContainer> open(const std::string &id);
  int read_at(const CardImageInfo &info, size_t position, uint8_t *buffer, size_t size);
  void close_reader(const std::string &id);

  static bool id_valid(const std::string &id);
  static std::string normalize_name(const std::string &value);
  static size_t record_size(size_t image_size);

 protected:
  struct CardImageHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc32;
    char id[48];
    char name[48];
    uint8_t padding[16];
  };
  static_assert(sizeof(CardImageHeader) == 128, "Card image header must remain 128 bytes");

  const esp_partition_t *partition_();
  void ensure_index_();
  bool read_header_(size_t offset, CardImageHeader &header);
  bool header_valid_(const CardImageHeader &header, size_t offset) const;
  bool verify_record_(const CardImageHeader &header, size_t offset);
  int find_empty_offset_(size_t image_size);
  std::string next_id_();
  CardImageInfo info_from_header_(const CardImageHeader &header, size_t offset) const;
  int find_index_(const std::string &id) const;
  static uint32_t crc32_update_(uint32_t crc, const uint8_t *data, size_t size);

  const esp_partition_t *partition_cache_{nullptr};
  bool partition_attempted_{false};
  bool index_loaded_{false};
  std::vector<CardImageInfo> images_{};
  std::vector<std::pair<std::string, size_t>> readers_{};
};

}  // namespace esphome::card_image_store
