#include "transfer/chunker.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <stdexcept>

#include "identity/identity.h"

namespace remboard {

int32_t total_chunks_for_size(int64_t size_bytes, int32_t chunk_size) {
  if (size_bytes <= 0) return size_bytes == 0 ? 1 : 0;
  return static_cast<int32_t>((size_bytes + chunk_size - 1) / chunk_size);
}

std::vector<uint8_t> read_file_chunk(const std::string& file_path,
                                      int32_t index, int32_t chunk_size) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open file for reading: " + file_path);
  }
  in.seekg(static_cast<std::streamoff>(index) * chunk_size);
  std::vector<uint8_t> buf(chunk_size);
  in.read(reinterpret_cast<char*>(buf.data()), chunk_size);
  buf.resize(static_cast<size_t>(in.gcount()));
  return buf;
}

std::string sha256_file_hex(const std::string& file_path) {
  static const int init_rc = sodium_init();
  (void)init_rc;

  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open file for hashing: " + file_path);
  }

  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);

  std::array<char, 64 * 1024> buf{};
  while (in) {
    in.read(buf.data(), buf.size());
    std::streamsize n = in.gcount();
    if (n > 0) {
      crypto_hash_sha256_update(
          &state, reinterpret_cast<const unsigned char*>(buf.data()),
          static_cast<unsigned long long>(n));
    }
  }

  std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256_final(&state, digest.data());
  return to_hex(std::vector<uint8_t>(digest.begin(), digest.end()));
}

FileReceiveState::FileReceiveState(std::string transfer_id,
                                    std::string dest_path,
                                    int32_t total_chunks, int32_t chunk_size)
    : transfer_id_(std::move(transfer_id)),
      dest_path_(std::move(dest_path)),
      total_chunks_(total_chunks),
      chunk_size_(chunk_size),
      received_mask_(static_cast<size_t>(total_chunks), false) {
  file_.open(dest_path_,
             std::ios::binary | std::ios::out | std::ios::trunc);
  if (!file_) {
    throw std::runtime_error("cannot create staging file: " + dest_path_);
  }
}

void FileReceiveState::write_chunk(int32_t index,
                                    const std::vector<uint8_t>& data) {
  if (index < 0 || index >= total_chunks_) {
    throw std::runtime_error("chunk index out of range");
  }
  file_.seekp(static_cast<std::streamoff>(index) * chunk_size_);
  file_.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  file_.flush();
  if (!received_mask_[static_cast<size_t>(index)]) {
    received_mask_[static_cast<size_t>(index)] = true;
    ++chunks_received_;
  }
}

bool FileReceiveState::all_received() const {
  return chunks_received_ >= total_chunks_;
}

void FileReceiveState::close() {
  if (file_.is_open()) file_.close();
}

}  // namespace remboard
