/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VALKEYSEARCH_SRC_INDEXES_NUMERIC_H_
#define VALKEYSEARCH_SRC_INDEXES_NUMERIC_H_
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/any_invocable.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "src/indexes/index_base.h"
#include "src/query/predicate.h"
#include "src/rdb_io_stream.h"
#include "src/utils/segment_tree.h"
#include "src/utils/string_interning.h"
#include "vmsdk/src/redismodule.h"

namespace valkey_search::indexes {

template <typename T, typename Hasher = absl::Hash<T>,
          typename Equaler = std::equal_to<T>>
class BTreeNumericIndex {
 public:
  using SetType = absl::flat_hash_set<T, Hasher, Equaler>;
  using ConstIterator = absl::btree_map<double, SetType>::const_iterator;

  void Add(const T& value, double key) {
    btree_[key].insert(value);
    segment_tree_.Add(key);
  }

  void Modify(const T& value, double old_key, double new_key) {
    Remove(value, old_key);
    Add(value, new_key);
  }

  void Remove(const T& value, double key) {
    btree_[key].erase(value);
    if (btree_[key].empty()) {
      btree_.erase(key);
    }
    segment_tree_.Remove(key);
  }
  const absl::btree_map<double, SetType>& GetBtree() const { return btree_; }

  size_t GetCount(double start, double end, bool start_inclusive,
                  bool end_inclusive) {
    return segment_tree_.Count(start, end, start_inclusive, end_inclusive);
  }

 private:
  // Right now we have both BTree and Segment Tree. The BTree is used to
  // maintain the keys and the values. The segment tree is used to maintain the
  // count of the keys in the range.
  //
  // Note on overhead: SegmentTree is roughly 80 bytes per entry (40 B per node,
  // 2x nodes per entries with a balanced tree).
  //
  // TODO(b/373427882): Consider using a single data structure to maintain both
  // the keys and the count.
  absl::btree_map<double, SetType> btree_;
  utils::SegmentTree segment_tree_;
};

class Numeric : public IndexBase {
 public:
  explicit Numeric(const data_model::NumericIndex& numeric_index_proto);
  absl::StatusOr<bool> AddRecord(const InternedStringPtr& key,
                                 absl::string_view data) override
      ABSL_LOCKS_EXCLUDED(index_mutex_);
  absl::StatusOr<bool> RemoveRecord(
      const InternedStringPtr& key,
      DeletionType deletion_type = DeletionType::kNone) override
      ABSL_LOCKS_EXCLUDED(index_mutex_);
  absl::StatusOr<bool> ModifyRecord(const InternedStringPtr& key,
                                    absl::string_view data) override
      ABSL_LOCKS_EXCLUDED(index_mutex_);
  int RespondWithInfo(RedisModuleCtx* ctx) const override;
  bool IsTracked(const InternedStringPtr& key) const override;
  absl::Status SaveIndex(RDBOutputStream& rdb_stream) const override {
    return absl::OkStatus();
  }
  inline void ForEachTrackedKey(
      absl::AnyInvocable<void(const InternedStringPtr&)> fn) const override {
    absl::MutexLock lock(&index_mutex_);
    for (const auto& [key, _] : tracked_keys_) {
      fn(key);
    }
  }
  uint64_t GetRecordCount() const override;
  std::unique_ptr<data_model::Index> ToProto() const override;

  const double* GetValue(const InternedStringPtr& key) const
      ABSL_NO_THREAD_SAFETY_ANALYSIS;
  using BTreeNumericIndex =
      BTreeNumericIndex<InternedStringPtr, InternedStringPtrHash,
                        InternedStringPtrEqual>;
  using EntriesRange = std::pair<BTreeNumericIndex::ConstIterator,
                                 BTreeNumericIndex::ConstIterator>;
  class EntriesFetcherIterator : public EntriesFetcherIteratorBase {
   public:
    EntriesFetcherIterator(
        const EntriesRange& entries_range,
        const std::optional<EntriesRange>& additional_entries_range,
        const InternedStringSet* untracked_keys);
    bool Done() const override;
    void Next() override;
    const InternedStringPtr& operator*() const override;

   private:
    static bool NextKeys(
        const Numeric::EntriesRange& range,
        BTreeNumericIndex::ConstIterator& iter,
        std::optional<InternedStringSet::const_iterator>& keys_iter);
    const EntriesRange& entries_range_;
    BTreeNumericIndex::ConstIterator entries_iter_;
    std::optional<InternedStringSet::const_iterator> entry_keys_iter_;
    const std::optional<EntriesRange>& additional_entries_range_;
    BTreeNumericIndex::ConstIterator additional_entries_iter_;
    std::optional<InternedStringSet::const_iterator>
        additional_entry_keys_iter_;
    const InternedStringSet* untracked_keys_;
    std::optional<InternedStringSet::const_iterator> untracked_keys_iter_;
  };

  class EntriesFetcher : public EntriesFetcherBase {
   public:
    EntriesFetcher(
        const EntriesRange& entries_range, size_t size,
        std::optional<EntriesRange> additional_entries_range = std::nullopt,
        const InternedStringSet* untracked_keys = nullptr)
        : entries_range_(entries_range),
          size_(size),
          additional_entries_range_(additional_entries_range),
          untracked_keys_(untracked_keys) {}
    virtual size_t Size() const override;
    std::unique_ptr<EntriesFetcherIteratorBase> Begin() override;

   private:
    EntriesRange entries_range_;
    size_t size_{0};
    std::optional<EntriesRange> additional_entries_range_;
    const InternedStringSet* untracked_keys_;
  };

  virtual std::unique_ptr<EntriesFetcher> Search(
      const query::NumericPredicate& predicate,
      bool negate) const ABSL_NO_THREAD_SAFETY_ANALYSIS;

 private:
  mutable absl::Mutex index_mutex_;
  InternedStringMap<double> tracked_keys_ ABSL_GUARDED_BY(index_mutex_);
  // untracked keys is needed to support negate filtering
  InternedStringSet untracked_keys_ ABSL_GUARDED_BY(index_mutex_);
  std::unique_ptr<BTreeNumericIndex> index_ ABSL_GUARDED_BY(index_mutex_);
};
}  // namespace valkey_search::indexes

#endif  // VALKEYSEARCH_SRC_INDEXES_NUMERIC_H_
