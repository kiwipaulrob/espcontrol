#include "card_image_store.h"

#include "esphome/core/log.h"

#include "esp_random.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace esphome::card_image_store {

static const char *const TAG = "card_image_store";
static constexpr uint32_t CARD_IMAGE_MAGIC = 0x43494D47;  // CIMG
static constexpr esp_partition_subtype_t CARD_IMAGE_PARTITION_SUBTYPE =
    static_cast<esp_partition_subtype_t>(0x40);

CardImageReader::CardImageReader(CardImageStore *store, const CardImageInfo &info)
    : store_(store), info_(info) {
  this->status_code = 200;
  this->content_length = info.size;
}

int CardImageReader::read(uint8_t *buf, size_t max_len) {
  if (this->ended_ || this->store_ == nullptr || this->position_ >= this->info_.size) return 0;
  size_t count = std::min(max_len, this->info_.size - this->position_);
  int result = this->store_->read_at(this->info_, this->position_, buf, count);
  if (result > 0) {
    this->position_ += static_cast<size_t>(result);
    this->bytes_read_ += static_cast<size_t>(result);
  }
  return result;
}

void CardImageReader::end() {
  if (this->ended_) return;
  this->ended_ = true;
  if (this->store_ != nullptr) this->store_->close_reader(this->info_.id);
  this->store_ = nullptr;
}

CardImageStore &CardImageStore::instance() {
  static CardImageStore store;
  return store;
}

const esp_partition_t *CardImageStore::partition_() {
  if (this->partition_attempted_) return this->partition_cache_;
  this->partition_attempted_ = true;
  this->partition_cache_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, CARD_IMAGE_PARTITION_SUBTYPE, "card_images");
  if (this->partition_cache_ == nullptr) {
    ESP_LOGW(TAG, "Dedicated card_images partition not found");
  } else if (this->partition_cache_->size < CARD_IMAGE_FLASH_SECTOR_SIZE) {
    ESP_LOGE(TAG, "card_images partition is too small");
    this->partition_cache_ = nullptr;
  }
  return this->partition_cache_;
}

bool CardImageStore::available() { return this->partition_() != nullptr; }
size_t CardImageStore::capacity() { return this->partition_() ? this->partition_()->size : 0; }

size_t CardImageStore::record_size(size_t image_size) {
  size_t bytes = sizeof(CardImageHeader) + image_size;
  return ((bytes + CARD_IMAGE_FLASH_SECTOR_SIZE - 1) / CARD_IMAGE_FLASH_SECTOR_SIZE) *
         CARD_IMAGE_FLASH_SECTOR_SIZE;
}

bool CardImageStore::id_valid(const std::string &id) {
  if (id.empty() || id.size() > 40) return false;
  for (char ch : id) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
  }
  return true;
}

std::string CardImageStore::normalize_name(const std::string &value) {
  std::string out;
  out.reserve(std::min(value.size(), CARD_IMAGE_NAME_MAX_LENGTH));
  bool previous_space = false;
  for (char raw : value) {
    unsigned char ch = static_cast<unsigned char>(raw);
    if (ch < 0x20 || ch == 0x7F || raw == ',' || raw == ';') continue;
    if (std::isspace(ch)) {
      if (!out.empty() && !previous_space) out.push_back(' ');
      previous_space = true;
      continue;
    }
    out.push_back(raw);
    previous_space = false;
    if (out.size() >= CARD_IMAGE_NAME_MAX_LENGTH) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

uint32_t CardImageStore::crc32_update_(uint32_t crc, const uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  return crc;
}

bool CardImageStore::read_header_(size_t offset, CardImageHeader &header) {
  const esp_partition_t *partition = this->partition_();
  if (partition == nullptr || offset + sizeof(header) > partition->size) return false;
  return esp_partition_read(partition, offset, &header, sizeof(header)) == ESP_OK;
}

bool CardImageStore::header_valid_(const CardImageHeader &header, size_t offset) const {
  if (header.magic != CARD_IMAGE_MAGIC || header.version != CARD_IMAGE_FORMAT_VERSION) return false;
  if (header.size == 0 || header.size > CARD_IMAGE_MAX_BYTES) return false;
  if (header.id[sizeof(header.id) - 1] != '\0' || header.name[sizeof(header.name) - 1] != '\0') return false;
  if (!id_valid(header.id)) return false;
  return this->partition_cache_ != nullptr && offset + record_size(header.size) <= this->partition_cache_->size;
}

bool CardImageStore::verify_record_(const CardImageHeader &header, size_t offset) {
  uint8_t buffer[1024];
  uint32_t crc = 0xFFFFFFFFu;
  size_t remaining = header.size;
  size_t position = 0;
  while (remaining > 0) {
    size_t count = std::min(remaining, sizeof(buffer));
    if (esp_partition_read(this->partition_(), offset + sizeof(CardImageHeader) + position,
                           buffer, count) != ESP_OK) return false;
    crc = crc32_update_(crc, buffer, count);
    position += count;
    remaining -= count;
  }
  return (crc ^ 0xFFFFFFFFu) == header.crc32;
}

CardImageInfo CardImageStore::info_from_header_(const CardImageHeader &header, size_t offset) const {
  CardImageInfo info;
  info.id = header.id;
  info.name = normalize_name(header.name);
  if (info.name.empty()) info.name = info.id;
  info.size = header.size;
  info.offset = offset;
  info.crc32 = header.crc32;
  return info;
}

void CardImageStore::ensure_index_() {
  if (this->index_loaded_) return;
  this->index_loaded_ = true;
  this->images_.clear();
  const esp_partition_t *partition = this->partition_();
  if (partition == nullptr) return;
  CardImageHeader header{};
  for (size_t offset = 0; offset + sizeof(header) <= partition->size;
       offset += CARD_IMAGE_FLASH_SECTOR_SIZE) {
    if (!this->read_header_(offset, header) || !this->header_valid_(header, offset)) continue;
    if (!this->verify_record_(header, offset)) {
      ESP_LOGW(TAG, "Ignoring card image with invalid CRC at 0x%zx", offset);
      continue;
    }
    this->images_.push_back(this->info_from_header_(header, offset));
  }
}

const std::vector<CardImageInfo> &CardImageStore::list() {
  this->ensure_index_();
  return this->images_;
}

int CardImageStore::find_index_(const std::string &id) const {
  for (size_t i = 0; i < this->images_.size(); i++) if (this->images_[i].id == id) return static_cast<int>(i);
  return -1;
}

bool CardImageStore::find(const std::string &id, CardImageInfo &out) {
  this->ensure_index_();
  int index = this->find_index_(id);
  if (index < 0) return false;
  out = this->images_[index];
  return true;
}

size_t CardImageStore::used_bytes() {
  this->ensure_index_();
  size_t used = 0;
  for (const auto &image : this->images_) used += record_size(image.size);
  return used;
}

size_t CardImageStore::free_bytes() {
  size_t total = this->capacity();
  size_t used = this->used_bytes();
  return total > used ? total - used : 0;
}

int CardImageStore::find_empty_offset_(size_t image_size) {
  this->ensure_index_();
  size_t total_sectors = this->capacity() / CARD_IMAGE_FLASH_SECTOR_SIZE;
  size_t needed = record_size(image_size) / CARD_IMAGE_FLASH_SECTOR_SIZE;
  std::vector<uint8_t> used(total_sectors, 0);
  for (const auto &image : this->images_) {
    size_t first = image.offset / CARD_IMAGE_FLASH_SECTOR_SIZE;
    size_t count = record_size(image.size) / CARD_IMAGE_FLASH_SECTOR_SIZE;
    for (size_t i = 0; i < count && first + i < used.size(); i++) used[first + i] = 1;
  }
  size_t run = 0;
  for (size_t sector = 0; sector < total_sectors; sector++) {
    run = used[sector] ? 0 : run + 1;
    if (run >= needed) return static_cast<int>((sector + 1 - needed) * CARD_IMAGE_FLASH_SECTOR_SIZE);
  }
  return -1;
}

std::string CardImageStore::next_id_() {
  for (int attempt = 0; attempt < 8; attempt++) {
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    char id[37];
    std::snprintf(id, sizeof(id),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  random[0], random[1], random[2], random[3], random[4], random[5], random[6], random[7],
                  random[8], random[9], random[10], random[11], random[12], random[13], random[14], random[15]);
    if (this->find_index_(id) < 0) return id;
  }
  return "";
}

esp_err_t CardImageStore::begin_upload(size_t size, CardImageUpload &upload) {
  if (!this->available()) return ESP_ERR_NOT_FOUND;
  if (size == 0 || size > CARD_IMAGE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
  int offset = this->find_empty_offset_(size);
  if (offset < 0) return ESP_ERR_NO_MEM;
  upload = {};
  upload.id = this->next_id_();
  if (upload.id.empty()) return ESP_FAIL;
  upload.offset = static_cast<size_t>(offset);
  upload.size = size;
  upload.record_size = record_size(size);
  return esp_partition_erase_range(this->partition_(), upload.offset, upload.record_size);
}

esp_err_t CardImageStore::write_upload(CardImageUpload &upload, const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0 || upload.written + size > upload.size) return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < size; i++) {
    if (upload.written + i < 2) upload.first_bytes[upload.written + i] = data[i];
    upload.last_bytes[0] = upload.last_bytes[1];
    upload.last_bytes[1] = data[i];
  }
  esp_err_t err = esp_partition_write(this->partition_(),
      upload.offset + sizeof(CardImageHeader) + upload.written, data, size);
  if (err != ESP_OK) return err;
  upload.crc32 = crc32_update_(upload.crc32, data, size);
  upload.written += size;
  return ESP_OK;
}

esp_err_t CardImageStore::commit_upload(CardImageUpload &upload, CardImageInfo &out) {
  if (upload.written != upload.size) return ESP_ERR_INVALID_SIZE;
  if (upload.first_bytes[0] != 0xFF || upload.first_bytes[1] != 0xD8 ||
      upload.last_bytes[0] != 0xFF || upload.last_bytes[1] != 0xD9) return ESP_ERR_INVALID_ARG;
  CardImageHeader header{};
  header.magic = CARD_IMAGE_MAGIC;
  header.version = CARD_IMAGE_FORMAT_VERSION;
  header.size = upload.size;
  header.crc32 = upload.crc32 ^ 0xFFFFFFFFu;
  strlcpy(header.id, upload.id.c_str(), sizeof(header.id));
  strlcpy(header.name, upload.id.c_str(), sizeof(header.name));
  esp_err_t err = esp_partition_write(this->partition_(), upload.offset, &header, sizeof(header));
  if (err != ESP_OK) return err;
  out = this->info_from_header_(header, upload.offset);
  this->images_.push_back(out);
  upload = {};
  return ESP_OK;
}

void CardImageStore::abort_upload(CardImageUpload &upload) {
  if (this->partition_() && upload.record_size) {
    esp_partition_erase_range(this->partition_(), upload.offset, upload.record_size);
  }
  upload = {};
}

esp_err_t CardImageStore::rename(const std::string &id, const std::string &name, CardImageInfo &out) {
  this->ensure_index_();
  int index = this->find_index_(id);
  if (index < 0) return ESP_ERR_NOT_FOUND;
  CardImageHeader header{};
  if (!this->read_header_(this->images_[index].offset, header) || !this->header_valid_(header, this->images_[index].offset)) {
    return ESP_ERR_INVALID_STATE;
  }
  strlcpy(header.name, normalize_name(name).c_str(), sizeof(header.name));
  uint8_t sector[CARD_IMAGE_FLASH_SECTOR_SIZE];
  esp_err_t err = esp_partition_read(this->partition_(), this->images_[index].offset, sector, sizeof(sector));
  if (err == ESP_OK) memcpy(sector, &header, sizeof(header));
  if (err == ESP_OK) err = esp_partition_erase_range(this->partition_(), this->images_[index].offset, sizeof(sector));
  if (err == ESP_OK) err = esp_partition_write(this->partition_(), this->images_[index].offset, sector, sizeof(sector));
  if (err != ESP_OK) return err;
  out = this->info_from_header_(header, this->images_[index].offset);
  this->images_[index] = out;
  return ESP_OK;
}

esp_err_t CardImageStore::erase(const std::string &id) {
  this->ensure_index_();
  int index = this->find_index_(id);
  if (index < 0) return ESP_ERR_NOT_FOUND;
  for (const auto &reader : this->readers_) if (reader.first == id && reader.second > 0) return ESP_ERR_INVALID_STATE;
  const auto image = this->images_[index];
  esp_err_t err = esp_partition_erase_range(this->partition_(), image.offset, record_size(image.size));
  if (err == ESP_OK) this->images_.erase(this->images_.begin() + index);
  return err;
}

std::shared_ptr<http_request::HttpContainer> CardImageStore::open(const std::string &id) {
  CardImageInfo info;
  if (!this->find(id, info)) return nullptr;
  auto it = std::find_if(this->readers_.begin(), this->readers_.end(), [&id](const auto &entry) { return entry.first == id; });
  if (it == this->readers_.end()) this->readers_.push_back({id, 1}); else it->second++;
  return std::make_shared<CardImageReader>(this, info);
}

int CardImageStore::read_at(const CardImageInfo &info, size_t position, uint8_t *buffer, size_t size) {
  if (buffer == nullptr || position >= info.size) return 0;
  size = std::min(size, info.size - position);
  esp_err_t err = esp_partition_read(this->partition_(),
      info.offset + sizeof(CardImageHeader) + position, buffer, size);
  return err == ESP_OK ? static_cast<int>(size) : -1;
}

void CardImageStore::close_reader(const std::string &id) {
  auto it = std::find_if(this->readers_.begin(), this->readers_.end(), [&id](const auto &entry) { return entry.first == id; });
  if (it == this->readers_.end()) return;
  if (it->second > 1) it->second--; else this->readers_.erase(it);
}

}  // namespace esphome::card_image_store
