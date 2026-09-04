#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace remboard {

constexpr int32_t kChunkSizeBytes = 256 * 1024;

int32_t total_chunks_for_size(int64_t size_bytes,
                               int32_t chunk_size = kChunkSizeBytes);

// Reads chunk `index` (0-based) of `file_path`. Throws std::runtime_error on
// IO error.
std::vector<uint8_t> read_file_chunk(const std::string& file_path,
                                      int32_t index,
                                      int32_t chunk_size = kChunkSizeBytes);

std::string sha256_file_hex(const std::string& file_path);

// Accumulates chunks (possibly out of order) for one incoming file transfer
// into a staging file at dest_path, tracking completeness. Not thread-safe;
// callers serialize access (Core does so by only touching it from its
// single message-processing context).
class FileReceiveState {
 public:
  FileReceiveState(std::string transfer_id, std::string dest_path,
                    int32_t total_chunks, int32_t chunk_size);

  void write_chunk(int32_t index, const std::vector<uint8_t>& data);
  bool all_received() const;
  int32_t chunks_received() const { return chunks_received_; }
  int32_t total_chunks() const { return total_chunks_; }
  const std::string& dest_path() const { return dest_path_; }
  const std::string& transfer_id() const { return transfer_id_; }

  void close();

 private:
  std::string transfer_id_;
  std::string dest_path_;
  int32_t total_chunks_;
  int32_t chunk_size_;
  int32_t chunks_received_ = 0;
  std::vector<bool> received_mask_;
  std::fstream file_;
};

}  // namespace remboard
