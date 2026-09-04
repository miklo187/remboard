#include "transfer/chunker.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

namespace {

namespace fs = std::filesystem;

class ChunkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           fs::path("remboard_chunker_test_" +
                     std::to_string(std::random_device{}()));
    fs::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  std::string write_file(const std::string& name,
                          const std::vector<uint8_t>& content) {
    std::string path = (dir_ / name).string();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(content.data()), content.size());
    out.close();
    return path;
  }

  fs::path dir_;
};

}  // namespace

TEST(TotalChunksForSize, ExactMultiple) {
  EXPECT_EQ(remboard::total_chunks_for_size(0, 10), 1);
  EXPECT_EQ(remboard::total_chunks_for_size(10, 10), 1);
  EXPECT_EQ(remboard::total_chunks_for_size(11, 10), 2);
  EXPECT_EQ(remboard::total_chunks_for_size(20, 10), 2);
  EXPECT_EQ(remboard::total_chunks_for_size(21, 10), 3);
}

TEST(TotalChunksForSize, DefaultChunkSize) {
  EXPECT_EQ(remboard::total_chunks_for_size(remboard::kChunkSizeBytes),
            1);
  EXPECT_EQ(remboard::total_chunks_for_size(remboard::kChunkSizeBytes + 1),
            2);
}

TEST_F(ChunkerTest, ReadFileChunkReturnsExactBytes) {
  std::vector<uint8_t> content(25);
  for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<uint8_t>(i);
  std::string path = write_file("data.bin", content);

  auto chunk0 = remboard::read_file_chunk(path, 0, 10);
  auto chunk1 = remboard::read_file_chunk(path, 1, 10);
  auto chunk2 = remboard::read_file_chunk(path, 2, 10);

  ASSERT_EQ(chunk0.size(), 10u);
  ASSERT_EQ(chunk1.size(), 10u);
  ASSERT_EQ(chunk2.size(), 5u);  // last chunk is short

  EXPECT_EQ(chunk0, std::vector<uint8_t>(content.begin(), content.begin() + 10));
  EXPECT_EQ(chunk1, std::vector<uint8_t>(content.begin() + 10, content.begin() + 20));
  EXPECT_EQ(chunk2, std::vector<uint8_t>(content.begin() + 20, content.end()));
}

TEST_F(ChunkerTest, Sha256FileHexKnownVectors) {
  std::string empty_path = write_file("empty.bin", {});
  EXPECT_EQ(remboard::sha256_file_hex(empty_path),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  std::vector<uint8_t> abc = {'a', 'b', 'c'};
  std::string abc_path = write_file("abc.bin", abc);
  EXPECT_EQ(remboard::sha256_file_hex(abc_path),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(ChunkerTest, FileReceiveStateAssemblesOutOfOrderChunks) {
  std::vector<uint8_t> chunk_a(10, 'A');
  std::vector<uint8_t> chunk_b(10, 'B');
  std::vector<uint8_t> chunk_c(5, 'C');

  std::string dest = (dir_ / "reassembled.bin").string();
  remboard::FileReceiveState state("transfer-1", dest, 3, 10);

  EXPECT_FALSE(state.all_received());
  EXPECT_EQ(state.chunks_received(), 0);

  // Deliberately out of order.
  state.write_chunk(2, chunk_c);
  EXPECT_FALSE(state.all_received());
  state.write_chunk(0, chunk_a);
  state.write_chunk(1, chunk_b);
  EXPECT_TRUE(state.all_received());
  EXPECT_EQ(state.chunks_received(), 3);

  state.close();

  std::ifstream in(dest, std::ios::binary);
  std::vector<uint8_t> result((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), chunk_a.begin(), chunk_a.end());
  expected.insert(expected.end(), chunk_b.begin(), chunk_b.end());
  expected.insert(expected.end(), chunk_c.begin(), chunk_c.end());
  EXPECT_EQ(result, expected);
}

TEST_F(ChunkerTest, FileReceiveStateDuplicateChunkDoesNotDoubleCount) {
  std::string dest = (dir_ / "dup.bin").string();
  remboard::FileReceiveState state("transfer-2", dest, 2, 4);
  std::vector<uint8_t> chunk(4, 'X');

  state.write_chunk(0, chunk);
  state.write_chunk(0, chunk);  // re-delivery of the same chunk
  EXPECT_EQ(state.chunks_received(), 1);

  state.write_chunk(1, chunk);
  EXPECT_EQ(state.chunks_received(), 2);
  EXPECT_TRUE(state.all_received());
}
