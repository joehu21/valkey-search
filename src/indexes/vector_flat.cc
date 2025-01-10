// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/indexes/vector_flat.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>  // NOLINT(build/c++11)
#include <queue>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "src/attribute_data_type.h"
#include "src/indexes/index_base.h"
#include "src/indexes/vector_base.h"
#include "src/metrics.h"
#include "src/rdb_io_stream.h"
#include "src/utils/string_interning.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"
#include "vmsdk/src/status/status_macros.h"

// Note that the ordering matters here - we want to minimize the memory
// overrides to just the hnswlib code.
// clang-format off
#include "vmsdk/src/memory_allocation_overrides.h"  // IWYU pragma: keep
#include "third_party/hnswlib/bruteforce.h"
#include "third_party/hnswlib/hnswlib.h"
// clang-format on

namespace valkey_search::indexes {

template <typename T>
absl::StatusOr<std::shared_ptr<VectorFlat<T>>> VectorFlat<T>::Create(
    const data_model::VectorIndex &vector_index_proto,
    absl::string_view attribute_identifier,
    data_model::AttributeDataType attribute_data_type) {
  try {
    auto index = std::shared_ptr<VectorFlat<T>>(
        new VectorFlat<T>(vector_index_proto.dimension_count(),
                          vector_index_proto.distance_metric(),
                          vector_index_proto.flat_algorithm().block_size(),
                          attribute_identifier, attribute_data_type));
    index->Init(vector_index_proto.dimension_count(),
                vector_index_proto.distance_metric(), index->space_);
    index->algo_ = std::make_unique<hnswlib::BruteforceSearch<T>>(
        index->space_.get(), vector_index_proto.initial_cap());
    return index;
  } catch (const std::exception &e) {
    ++Metrics::GetStats().flat_create_exceptions_cnt;
    return absl::InternalError(
        absl::StrCat("Error while creating a FLAT index: ", e.what()));
  }
}

template <typename T>
char *VectorFlat<T>::TrackVector(uint64_t internal_id,
                                 const InternedStringPtr &vector) {
  absl::MutexLock lock(&tracked_vectors_mutex_);
  tracked_vectors_[internal_id] = vector;
  return (char *)vector->Str().data();
}

template <typename T>
void VectorFlat<T>::UnTrackVector(uint64_t internal_id) {
  absl::MutexLock lock(&tracked_vectors_mutex_);
  tracked_vectors_.erase(internal_id);
}

template <typename T>
absl::StatusOr<std::shared_ptr<VectorFlat<T>>> VectorFlat<T>::LoadFromRDB(
    RedisModuleCtx *ctx, const AttributeDataType *attribute_data_type,
    const data_model::VectorIndex &vector_index_proto,
    RDBInputStream &rdb_stream, absl::string_view attribute_identifier) {
  try {
    auto index = std::shared_ptr<VectorFlat<T>>(new VectorFlat<T>(
        vector_index_proto.dimension_count(),
        vector_index_proto.distance_metric(),
        vector_index_proto.flat_algorithm().block_size(), attribute_identifier,
        attribute_data_type->ToProto()));
    index->Init(vector_index_proto.dimension_count(),
                vector_index_proto.distance_metric(), index->space_);
    index->algo_ =
        std::make_unique<hnswlib::BruteforceSearch<T>>(index->space_.get());
    VMSDK_RETURN_IF_ERROR(
        index->algo_->LoadIndex(rdb_stream, index->space_.get(), index.get()));
    if (vector_index_proto.has_tracked_keys()) {
      VMSDK_RETURN_IF_ERROR(index->LoadTrackedKeys(
          ctx, attribute_data_type, vector_index_proto.tracked_keys()));
      VMSDK_RETURN_IF_ERROR(
          index->ConsumeKeysAndInternalIdsForBackCompat(rdb_stream));
    } else {
      // Previous versions stored tracked keys in the index contents.
      VMSDK_RETURN_IF_ERROR(
          index->LoadKeysAndInternalIds(ctx, attribute_data_type, rdb_stream));
    }
    return index;
  } catch (const std::exception &e) {
    ++Metrics::GetStats().flat_create_exceptions_cnt;
    return absl::InternalError(
        absl::StrCat("Error while loading a FLAT index: ", e.what()));
  }
}

template <typename T>
VectorFlat<T>::VectorFlat(
    int dimensions, valkey_search::data_model::DistanceMetric distance_metric,
    uint32_t block_size, absl::string_view attribute_identifier,
    data_model::AttributeDataType attribute_data_type)
    : VectorBase(IndexerType::kFlat, dimensions, attribute_data_type,
                 attribute_identifier),
      block_size_(block_size) {}

template <typename T>
absl::Status VectorFlat<T>::ResizeIfFull() {
  {
    absl::ReaderMutexLock lock(&resize_mutex_);
    if (algo_->cur_element_count_ < GetCapacity()) {
      return absl::OkStatus();
    }
  }
  absl::WriterMutexLock lock(&resize_mutex_);
  std::unique_lock<std::mutex> index_lock(algo_->index_lock);
  if (algo_->cur_element_count_ == GetCapacity()) {
    VMSDK_LOG(WARNING, nullptr)
        << "Resizing FLAT Index, current size: " << GetCapacity()
        << ", expand by: " << block_size_;
    algo_->resizeIndex(GetCapacity() + block_size_);
  }
  return absl::OkStatus();
}

template <typename T>
absl::Status VectorFlat<T>::AddRecordImpl(uint64_t internal_id,
                                          absl::string_view record) {
  do {
    try {
      absl::ReaderMutexLock lock(&resize_mutex_);

      algo_->addPoint((T *)record.data(), internal_id);
    } catch (const std::exception &e) {
      ++Metrics::GetStats().flat_add_exceptions_cnt;
      std::string error_msg = e.what();
      if (absl::StrContains(
              error_msg,
              "The number of elements exceeds the specified limit")) {
        VMSDK_RETURN_IF_ERROR(ResizeIfFull());
        continue;
      }
      return absl::InternalError(
          absl::StrCat("Error while adding a record: ", e.what()));
    }
    return absl::OkStatus();
  } while (true);
}

template <typename T>
absl::StatusOr<bool> VectorFlat<T>::ModifyRecordImpl(uint64_t internal_id,
                                                     absl::string_view record) {
  absl::ReaderMutexLock lock(&resize_mutex_);
  std::unique_lock<std::mutex> index_lock(algo_->index_lock);
  auto found = algo_->dict_external_to_internal.find(internal_id);
  if (found == algo_->dict_external_to_internal.end()) {
    return absl::InternalError(
        absl::StrCat("Couldn't find internal id: ", internal_id));
  }
  absl::string_view saved_record(*(char **)(*algo_->data_)[found->second],
                                 dimensions_ * sizeof(T));
  if (saved_record == record) {
    return false;
  }
  memcpy((*algo_->data_)[found->second] + algo_->data_ptr_size_, &internal_id,
         sizeof(hnswlib::labeltype));
  *(char **)((*algo_->data_)[found->second]) = (char *)record.data();

  return true;
}

template <typename T>
absl::Status VectorFlat<T>::RemoveRecordImpl(uint64_t internal_id) {
  try {
    absl::ReaderMutexLock lock(&resize_mutex_);
    algo_->removePoint(internal_id);
  } catch (const std::exception &e) {
    ++Metrics::GetStats().flat_remove_exceptions_cnt;
    return absl::InternalError(
        absl::StrCat("Error while removing a FLAT record: ", e.what()));
  }
  return absl::OkStatus();
}

template <typename T>
absl::StatusOr<std::deque<Neighbor>> VectorFlat<T>::Search(
    absl::string_view query, uint64_t count,
    std::unique_ptr<hnswlib::BaseFilterFunctor> filter) {
  if (!IsValidSizeVector(query)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Error parsing vector similarity query: query vector blob size (",
        query.size(), ") does not match index's expected size (",
        dimensions_ * GetDataTypeSize(), ")."));
  }
  auto perform_search = [this, count, &filter](absl::string_view query)
      -> absl::StatusOr<std::priority_queue<std::pair<T, hnswlib::labeltype>>> {
    absl::ReaderMutexLock lock(&resize_mutex_);
    try {
      return algo_->searchKnn((T *)query.data(),
                              std::min(count, algo_->cur_element_count_),
                              filter.get());
    } catch (const std::exception &e) {
      Metrics::GetStats().flat_search_exceptions_cnt.fetch_add(
          1, std::memory_order_relaxed);
      return absl::InternalError(e.what());
    }
  };
  if (normalize_) {
    auto norm_record = NormalizeEmbedding(query, GetDataTypeSize());
    VMSDK_ASSIGN_OR_RETURN(
        auto search_result,
        perform_search(absl::string_view((const char *)norm_record.data(),
                                         norm_record.size())));
    return CreateReply(search_result);
  }
  VMSDK_ASSIGN_OR_RETURN(auto search_result, perform_search(query));
  return CreateReply(search_result);
}

template <typename T>
absl::StatusOr<std::pair<float, hnswlib::labeltype>>
VectorFlat<T>::ComputeDistanceFromRecordImpl(uint64_t internal_id,
                                             absl::string_view query) const {
  absl::ReaderMutexLock lock(&resize_mutex_);
  auto search = algo_->dict_external_to_internal.find(internal_id);
  if (search == algo_->dict_external_to_internal.end()) {
    return absl::InternalError(
        absl::StrCat("Couldn't find internal id: ", internal_id));
  }
  return (std::pair<float, hnswlib::labeltype>){
      algo_->fstdistfunc_((T *)query.data(),
                          *(char **)(*algo_->data_)[search->second],
                          algo_->dist_func_param_),
      internal_id};
}

template <typename T>
void VectorFlat<T>::ToProtoImpl(
    data_model::VectorIndex *vector_index_proto) const {
  data_model::VectorDataType data_type;
  if constexpr (std::is_same_v<T, float>) {
    data_type = data_model::VectorDataType::VECTOR_DATA_TYPE_FLOAT32;
  } else {
    DCHECK(false) << "Unsupported type: " << typeid(T).name();
    data_type = data_model::VectorDataType::VECTOR_DATA_TYPE_UNSPECIFIED;
  }
  vector_index_proto->set_vector_data_type(data_type);

  auto flat_algorithm_proto = std::make_unique<data_model::FlatAlgorithm>();
  flat_algorithm_proto->set_block_size(block_size_);
  vector_index_proto->set_allocated_flat_algorithm(
      flat_algorithm_proto.release());
}

template <typename T>
int VectorFlat<T>::RespondWithInfoImpl(RedisModuleCtx *ctx) const {
  RedisModule_ReplyWithSimpleString(ctx, "data_type");
  if constexpr (std::is_same_v<T, float>) {
    RedisModule_ReplyWithSimpleString(
        ctx,
        LookupKeyByValue(*kVectorDataTypeByStr,
                         data_model::VectorDataType::VECTOR_DATA_TYPE_FLOAT32)
            .data());
  } else {
    RedisModule_ReplyWithSimpleString(ctx, "UNKNOWN");
  }
  RedisModule_ReplyWithSimpleString(ctx, "algorithm");
  RedisModule_ReplyWithArray(ctx, 4);
  RedisModule_ReplyWithSimpleString(ctx, "name");
  RedisModule_ReplyWithSimpleString(
      ctx,
      LookupKeyByValue(*kVectorAlgoByStr,
                       data_model::VectorIndex::AlgorithmCase::kFlatAlgorithm)
          .data());
  RedisModule_ReplyWithSimpleString(ctx, "block_size");
  RedisModule_ReplyWithLongLong(ctx, block_size_);

  return 4;
}

template <typename T>
absl::Status VectorFlat<T>::SaveIndexImpl(RDBOutputStream &rdb_stream) const {
  absl::ReaderMutexLock lock(&resize_mutex_);
  return algo_->SaveIndex(rdb_stream);
}

template class VectorFlat<float>;

}  // namespace valkey_search::indexes
