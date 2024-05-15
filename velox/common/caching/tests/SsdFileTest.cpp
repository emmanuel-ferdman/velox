/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/common/caching/FileIds.h"
#include "velox/common/caching/SsdCache.h"
#include "velox/common/memory/Memory.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

#include <folly/executors/QueuedImmediateExecutor.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

using namespace facebook::velox;
using namespace facebook::velox::cache;

using facebook::velox::memory::MemoryAllocator;

// Represents an entry written to SSD.
struct TestEntry {
  FileCacheKey key;
  uint64_t ssdOffset;
  int32_t size;

  TestEntry(FileCacheKey _key, uint64_t _ssdOffset, int32_t _size)
      : key(_key), ssdOffset(_ssdOffset), size(_size) {}
};

class SsdFileTest : public testing::Test {
 protected:
  static constexpr int64_t kMB = 1 << 20;

  void SetUp() override {
    memory::MemoryManager::testingSetInstance({});
  }

  void TearDown() override {
    if (ssdFile_) {
      ssdFile_->testingDeleteFile();
    }
    if (cache_) {
      cache_->shutdown();
    }
  }

  void initializeCache(
      int64_t ssdBytes = 0,
      int64_t checkpointIntervalBytes = 0,
      bool disableFileCow = false) {
    // tmpfs does not support O_DIRECT, so turn this off for testing.
    FLAGS_ssd_odirect = false;
    cache_ = AsyncDataCache::create(memory::memoryManager()->allocator());
    fileName_ = StringIdLease(fileIds(), "fileInStorage");
    tempDirectory_ = exec::test::TempDirectoryPath::create();
    initializeSsdFile(ssdBytes, checkpointIntervalBytes, disableFileCow);
  }

  void initializeSsdFile(
      int64_t ssdBytes = 0,
      int64_t checkpointIntervalBytes = 0,
      bool disableFileCow = false) {
    ssdFile_ = std::make_unique<SsdFile>(
        fmt::format("{}/ssdtest", tempDirectory_->getPath()),
        0, // shardId
        bits::roundUp(ssdBytes, SsdFile::kRegionSize) / SsdFile::kRegionSize,
        checkpointIntervalBytes,
        disableFileCow);
  }

  static void initializeContents(int64_t sequence, memory::Allocation& alloc) {
    bool first = true;
    for (int32_t i = 0; i < alloc.numRuns(); ++i) {
      memory::Allocation::PageRun run = alloc.runAt(i);
      int64_t* ptr = reinterpret_cast<int64_t*>(run.data());
      int32_t numWords =
          run.numPages() * memory::AllocationTraits::kPageSize / sizeof(void*);
      for (int32_t offset = 0; offset < numWords; offset++) {
        if (first) {
          ptr[offset] = sequence;
          first = false;
        } else {
          ptr[offset] = offset + sequence;
        }
      }
    }
  }

  // Checks that the contents are consistent with what is set in
  // initializeContents.
  static void checkContents(const memory::Allocation& alloc, int32_t numBytes) {
    bool first = true;
    int64_t sequence;
    int32_t bytesChecked = sizeof(int64_t);
    for (int32_t i = 0; i < alloc.numRuns(); ++i) {
      memory::Allocation::PageRun run = alloc.runAt(i);
      int64_t* ptr = reinterpret_cast<int64_t*>(run.data());
      int32_t numWords =
          run.numPages() * memory::AllocationTraits::kPageSize / sizeof(void*);
      for (int32_t offset = 0; offset < numWords; offset++) {
        if (first) {
          sequence = ptr[offset];
          first = false;
        } else {
          bytesChecked += sizeof(int64_t);
          if (bytesChecked >= numBytes) {
            return;
          }
          ASSERT_EQ(ptr[offset], offset + sequence);
        }
      }
    }
  }

  // Gets consecutive entries from file 'fileId' starting at 'startOffset'  with
  // sizes between 'minSize' and 'maxSize'. Sizes start at 'minSize' and double
  // each time and go back to 'minSize' after exceeding 'maxSize'. This stops
  // after the total size has exceeded 'totalSize'. The entries are returned as
  // pins. The pins are exclusive for newly created entries and shared for
  // existing ones. New entries are deterministically  initialized from 'fileId'
  // and the entry's offset.
  std::vector<CachePin> makePins(
      uint64_t fileId,
      uint64_t startOffset,
      int32_t minSize,
      int32_t maxSize,
      int64_t totalSize) {
    auto offset = startOffset;
    int64_t bytesFromCache = 0;
    auto size = minSize;
    std::vector<CachePin> pins;
    while (bytesFromCache < totalSize) {
      pins.push_back(
          cache_->findOrCreate(RawFileCacheKey{fileId, offset}, size, nullptr));
      bytesFromCache += size;
      EXPECT_FALSE(pins.back().empty());
      auto entry = pins.back().entry();
      if (entry && entry->isExclusive()) {
        initializeContents(fileId + offset, entry->data());
      }
      offset += size;
      size *= 2;
      if (size > maxSize) {
        size = minSize;
      }
    }
    return pins;
  }

  std::vector<SsdPin> pinAllRegions(const std::vector<TestEntry>& entries) {
    std::vector<SsdPin> pins;
    int32_t lastRegion = -1;
    for (auto& entry : entries) {
      if (entry.ssdOffset / SsdFile::kRegionSize != lastRegion) {
        lastRegion = entry.ssdOffset / SsdFile::kRegionSize;
        pins.push_back(
            ssdFile_->find(RawFileCacheKey{fileName_.id(), entry.key.offset}));
        EXPECT_FALSE(pins.back().empty());
      }
    }
    return pins;
  }

  void readAndCheckPins(const std::vector<CachePin>& pins) {
    std::vector<SsdPin> ssdPins;
    ssdPins.reserve(pins.size());
    for (auto& pin : pins) {
      ssdPins.push_back(ssdFile_->find(
          RawFileCacheKey{fileName_.id(), pin.entry()->key().offset}));
      EXPECT_FALSE(ssdPins.back().empty());
    }
    ssdFile_->load(ssdPins, pins);
    for (auto& pin : pins) {
      checkContents(pin.entry()->data(), pin.entry()->size());
    }
  }

  void checkEvictionBlocked(
      std::vector<TestEntry>& allEntries,
      uint64_t ssdSize) {
    auto ssdPins = pinAllRegions(allEntries);
    auto pins = makePins(fileName_.id(), ssdSize, 4096, 2048 * 1025, 62 * kMB);
    ssdFile_->write(pins);
    // Only Some pins get written because space cannot be cleared
    // because all regions are pinned. The file will not give out new
    // pins so that this situation is not continued
    EXPECT_TRUE(
        ssdFile_->find(RawFileCacheKey{fileName_.id(), ssdSize}).empty());
    int32_t numWritten = 0;
    for (auto& pin : pins) {
      if (pin.entry()->ssdFile()) {
        ++numWritten;
        allEntries.emplace_back(
            pin.entry()->key(), pin.entry()->ssdOffset(), pin.entry()->size());
      }
    }
    EXPECT_LT(numWritten, pins.size());
    // vector::clear() does not guarantee the release order; we need to clear
    // the pins in the correct order explicitly.
    for (auto& pin : ssdPins) {
      pin.clear();
    }
    ssdPins.clear();

    // The pins were cleared and the file is no longer suspended. Check that the
    // entry that was not found is found now.
    EXPECT_FALSE(
        ssdFile_->find(RawFileCacheKey{fileName_.id(), ssdSize}).empty());

    pins.erase(pins.begin(), pins.begin() + numWritten);
    ssdFile_->write(pins);
    for (auto& pin : pins) {
      if (pin.entry()->ssdFile()) {
        allEntries.emplace_back(
            pin.entry()->key(), pin.entry()->ssdOffset(), pin.entry()->size());
      }
    }
    // Clear the pins and read back. Clear must complete before
    // makePins so that there are no pre-existing exclusive pins for the same
    // key.
    pins.clear();
    pins = makePins(fileName_.id(), ssdSize, 4096, 2048 * 1025, 62 * kMB);
    readAndCheckPins(pins);
  }

  // Reads back the found entries and check their contents, return the number of
  // entries found.
  int32_t checkEntries(const std::vector<TestEntry>& entries) {
    int32_t numFound = 0;
    for (auto& entry : entries) {
      std::vector<CachePin> pins;
      pins.push_back(cache_->findOrCreate(
          RawFileCacheKey{fileName_.id(), entry.key.offset},
          entry.size,
          nullptr));
      if (pins.back().entry()->isExclusive()) {
        std::vector<SsdPin> ssdPins;
        ssdPins.push_back(
            ssdFile_->find(RawFileCacheKey{fileName_.id(), entry.key.offset}));
        if (!ssdPins.back().empty()) {
          ++numFound;
          ssdFile_->load(ssdPins, pins);
          checkContents(pins[0].entry()->data(), pins[0].entry()->size());
        }
      }
    }
    return numFound;
  }

  std::shared_ptr<exec::test::TempDirectoryPath> tempDirectory_;

  std::shared_ptr<AsyncDataCache> cache_;
  StringIdLease fileName_;

  std::unique_ptr<SsdFile> ssdFile_;
};

TEST_F(SsdFileTest, writeAndRead) {
  constexpr int64_t kSsdSize = 16 * SsdFile::kRegionSize;
  std::vector<TestEntry> allEntries;
  initializeCache(kSsdSize);
  FLAGS_ssd_verify_write = true;
  for (auto startOffset = 0; startOffset <= kSsdSize - SsdFile::kRegionSize;
       startOffset += SsdFile::kRegionSize) {
    auto pins =
        makePins(fileName_.id(), startOffset, 4096, 2048 * 1025, 62 * kMB);
    ssdFile_->write(pins);
    for (auto& pin : pins) {
      EXPECT_EQ(ssdFile_.get(), pin.entry()->ssdFile());
      allEntries.emplace_back(
          pin.entry()->key(), pin.entry()->ssdOffset(), pin.entry()->size());
    };
  }

  // The SsdFile is almost full and the memory cache has the last batch written
  // and a few entries from the batch before that.
  // We read back the same batches and check
  // contents.
  for (auto startOffset = 0; startOffset <= kSsdSize - SsdFile::kRegionSize;
       startOffset += SsdFile::kRegionSize) {
    auto pins =
        makePins(fileName_.id(), startOffset, 4096, 2048 * 1025, 62 * kMB);
    readAndCheckPins(pins);
  }
  checkEvictionBlocked(allEntries, kSsdSize);
  // We rewrite the cache with new entries.

  for (auto startOffset = kSsdSize + SsdFile::kRegionSize;
       startOffset <= kSsdSize * 2;
       startOffset += SsdFile::kRegionSize) {
    auto pins =
        makePins(fileName_.id(), startOffset, 4096, 2048 * 1025, 62 * kMB);
    ssdFile_->write(pins);
    for (auto& pin : pins) {
      EXPECT_EQ(ssdFile_.get(), pin.entry()->ssdFile());
      allEntries.emplace_back(
          pin.entry()->key(), pin.entry()->ssdOffset(), pin.entry()->size());
    }
  }

  // We check howmany entries are found. The earliest writes will have been
  // evicted. We read back the found entries and check their contents.
  int32_t numFound = 0;
  for (auto& entry : allEntries) {
    std::vector<CachePin> pins;

    pins.push_back(cache_->findOrCreate(
        RawFileCacheKey{fileName_.id(), entry.key.offset},
        entry.size,
        nullptr));
    if (pins.back().entry()->isExclusive()) {
      std::vector<SsdPin> ssdPins;

      ssdPins.push_back(
          ssdFile_->find(RawFileCacheKey{fileName_.id(), entry.key.offset}));
      if (!ssdPins.back().empty()) {
        ++numFound;
        ssdFile_->load(ssdPins, pins);
        checkContents(pins[0].entry()->data(), pins[0].entry()->size());
      }
    }
  }
}

TEST_F(SsdFileTest, checkpoint) {
  constexpr int64_t kSsdSize = 16 * SsdFile::kRegionSize;
  const int32_t checkpointIntervalBytes = 5 * SsdFile::kRegionSize;
  const auto fileNameAlt = StringIdLease(fileIds(), "fileInStorageAlt");
  FLAGS_ssd_verify_write = true;
  initializeCache(kSsdSize, checkpointIntervalBytes);

  std::vector<TestEntry> allEntries;
  for (auto startOffset = 0; startOffset <= kSsdSize - SsdFile::kRegionSize;
       startOffset += SsdFile::kRegionSize) {
    auto pins =
        makePins(fileName_.id(), startOffset, 4096, 2048 * 1025, 62 * kMB);
    // Each region has one entry from `fileNameAlt`.
    pins.push_back(cache_->findOrCreate(
        RawFileCacheKey{fileNameAlt.id(), (uint64_t)startOffset},
        1024,
        nullptr));
    ssdFile_->write(pins);
    for (auto& pin : pins) {
      EXPECT_EQ(ssdFile_.get(), pin.entry()->ssdFile());
      allEntries.emplace_back(
          pin.entry()->key(), pin.entry()->ssdOffset(), pin.entry()->size());
    };
    readAndCheckPins(pins);
  }
  const auto originalRegionScores = ssdFile_->testingCopyScores();
  EXPECT_EQ(originalRegionScores.size(), 16);

  // Re-initialize SSD file from checkpoint.
  ssdFile_->checkpoint(true);
  initializeSsdFile(kSsdSize, checkpointIntervalBytes);
  const auto recoveredRegionScores = ssdFile_->testingCopyScores();
  EXPECT_EQ(recoveredRegionScores.size(), 16);
  EXPECT_EQ(originalRegionScores, recoveredRegionScores);

  // Reconstruct cache pins and check the recovered content from cache file.
  for (auto startOffset = 0; startOffset <= kSsdSize - SsdFile::kRegionSize;
       startOffset += SsdFile::kRegionSize) {
    auto pins =
        makePins(fileName_.id(), startOffset, 4096, 2048 * 1025, 62 * kMB);
    pins.push_back(cache_->findOrCreate(
        RawFileCacheKey{fileNameAlt.id(), (uint64_t)startOffset},
        1024,
        nullptr));
    readAndCheckPins(pins);
  }
  // All entries can be found.
  auto numEntriesFound = checkEntries(allEntries);
  EXPECT_EQ(numEntriesFound, allEntries.size());

  // Test removeFileEntries.
  folly::F14FastSet<uint64_t> filesToRemove{fileName_.id()};
  folly::F14FastSet<uint64_t> filesRetained{};
  auto stats = ssdFile_->testingStats();
  EXPECT_EQ(stats.entriesAgedOut, 0);
  EXPECT_EQ(stats.regionsAgedOut, 0);
  EXPECT_EQ(stats.regionsEvicted, 0);

  // Block eviction.
  auto ssdPins = pinAllRegions(allEntries);
  ssdFile_->removeFileEntries(filesToRemove, filesRetained);
  EXPECT_EQ(ssdFile_->testingNumWritableRegions(), 0);
  EXPECT_EQ(filesRetained.size(), 1);
  numEntriesFound = checkEntries(allEntries);
  EXPECT_EQ(numEntriesFound, allEntries.size());
  auto prevStats = stats;
  stats = ssdFile_->testingStats();
  EXPECT_EQ(stats.entriesAgedOut - prevStats.entriesAgedOut, 0);
  EXPECT_EQ(stats.regionsAgedOut - prevStats.regionsAgedOut, 0);
  EXPECT_EQ(stats.regionsEvicted - prevStats.regionsEvicted, 0);

  // Unblock eviction.
  ssdPins.clear();
  filesRetained.clear();
  ssdFile_->removeFileEntries(filesToRemove, filesRetained);
  // All regions have been evicted and marked as writable.
  EXPECT_EQ(ssdFile_->testingNumWritableRegions(), 16);
  EXPECT_EQ(filesRetained.size(), 0);
  numEntriesFound = checkEntries(allEntries);
  EXPECT_EQ(numEntriesFound, 0);
  prevStats = stats;
  stats = ssdFile_->testingStats();
  EXPECT_EQ(
      stats.entriesAgedOut - prevStats.entriesAgedOut, allEntries.size() - 16);
  EXPECT_EQ(stats.regionsAgedOut - prevStats.regionsAgedOut, 16);
  EXPECT_EQ(stats.regionsEvicted - prevStats.regionsEvicted, 16);

  // Re-initialize SSD file from checkpoint. Since all regions were evicted, no
  // entries should be found.
  initializeSsdFile(kSsdSize, checkpointIntervalBytes);
  numEntriesFound = checkEntries(allEntries);
  EXPECT_EQ(numEntriesFound, 0);
}

#ifdef VELOX_SSD_FILE_TEST_SET_NO_COW_FLAG
TEST_F(SsdFileTest, disabledCow) {
  LOG(ERROR) << "here";
  constexpr int64_t kSsdSize = 16 * SsdFile::kRegionSize;
  initializeCache(kSsdSize, 0, true);
  EXPECT_TRUE(ssdFile_->testingIsCowDisabled());
}

TEST_F(SsdFileTest, notDisabledCow) {
  constexpr int64_t kSsdSize = 16 * SsdFile::kRegionSize;
  initializeCache(kSsdSize, 0, false);
  EXPECT_FALSE(ssdFile_->testingIsCowDisabled());
}
#endif // VELOX_SSD_FILE_TEST_SET_NO_COW_FLAG
