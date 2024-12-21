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

#include "src/indexes/vector_base.h"

#include <sys/types.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "third_party/hnswlib/hnswlib.h"
#include "third_party/hnswlib/space_ip.h"
#include "third_party/hnswlib/space_l2.h"
#include "src/attribute_data_type.h"
#include "src/index_schema.pb.h"
#include "src/indexes/index_base.h"
#include "src/indexes/numeric.h"
#include "src/indexes/tag.h"
#include "src/query/predicate.h"
#include "src/rdb_io_stream.h"
#include "src/utils/string_interning.h"
#include "src/vector_externalizer.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/redismodule.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/type_conversions.h"

namespace valkey_search {
constexpr float kDefaultMagnitude = -1.0f;

namespace {

template <typename T>
std::unique_ptr<hnswlib::SpaceInterface<T>> CreateSpace(
    int dimensions, valkey_search::data_model::DistanceMetric distance_metric) {
  if constexpr (std::is_same_v<T, float>) {
    if (distance_metric ==
            valkey_search::data_model::DistanceMetric::DISTANCE_METRIC_COSINE ||
        distance_metric ==
            valkey_search::data_model::DistanceMetric::DISTANCE_METRIC_IP) {
      return std::make_unique<hnswlib::InnerProductSpace>(dimensions);
    } else {
      return std::make_unique<hnswlib::L2Space>(dimensions);
    }
  }
  DCHECK(false) << "no matching spacer";
  return std::make_unique<hnswlib::L2Space>(dimensions);
}

}  // namespace

namespace indexes {
bool InlineVectorEvaluator::Evaluate(const query::Predicate &predicate,
                                     const InternedStringPtr &key) {
  key_ = &key;
  auto res = predicate.Evaluate(*this);
  key_ = nullptr;
  return res;
}

bool InlineVectorEvaluator::EvaluateTags(const query::TagPredicate &predicate) {
  bool case_sensitive = true;
  auto tags = predicate.GetIndex()->GetValue(*key_, case_sensitive);
  return predicate.Evaluate(tags, case_sensitive);
}

bool InlineVectorEvaluator::EvaluateNumeric(
    const query::NumericPredicate &predicate) {
  CHECK(key_);
  auto value = predicate.GetIndex()->GetValue(*key_);
  return predicate.Evaluate(value);
}

template <typename T>
T CopyAndNormalizeEmbedding(T *dst, T *src, size_t size) {
  T magnitude = 0.0f;
  for (size_t i = 0; i < size; i++) {
    magnitude += src[i] * src[i];
  }
  magnitude = std::sqrt(magnitude);
  if (magnitude == 0.0f) {
    return magnitude;
  }
  T norm = 1.0f / (magnitude);
  for (size_t i = 0; i < size; i++) {
    dst[i] = norm * src[i];
  }
  return magnitude;
}

std::vector<char> NormalizeEmbedding(absl::string_view record, size_t type_size,
                                     float *magnitude) {
  std::vector<char> ret(record.size());
  if (type_size == sizeof(float)) {
    float result = CopyAndNormalizeEmbedding(
        (float *)&ret[0], (float *)record.data(), ret.size() / sizeof(float));
    if (magnitude) {
      *magnitude = result;
    }
    return ret;
  }
  CHECK(false) << "unsupported type size";
}

template <typename T>
void VectorBase::Init(int dimensions,
                      valkey_search::data_model::DistanceMetric distance_metric,
                      std::unique_ptr<hnswlib::SpaceInterface<T>> &space) {
  space = CreateSpace<T>(dimensions, distance_metric);
  distance_metric_ = distance_metric;
  if (distance_metric ==
      valkey_search::data_model::DistanceMetric::DISTANCE_METRIC_COSINE) {
    normalize_ = true;
  }
}

std::shared_ptr<InternedString> VectorBase::InternVector(
     absl::string_view record,
    std::optional<float> &magnitude) {
  if (!IsValidSizeVector(record)) {
    return nullptr;
  }
  if (normalize_) {
    magnitude = kDefaultMagnitude;
    auto norm_record =
        NormalizeEmbedding(record, GetDataTypeSize(), &magnitude.value());
    return StringInternStore::Intern(
        absl::string_view((const char *)norm_record.data(), norm_record.size()),
        vector_allocator_.get());
  }
  return StringInternStore::Intern(record, vector_allocator_.get());
}

absl::StatusOr<bool> VectorBase::AddRecord(const InternedStringPtr &key,
                                           absl::string_view record) {
  std::optional<float> magnitude;
  auto interned_vector = InternVector(record, magnitude);
  if (!interned_vector) {
    return false;
  }
  VMSDK_ASSIGN_OR_RETURN(
      auto internal_id,
      TrackKey(key, magnitude.value_or(kDefaultMagnitude), interned_vector));
  absl::Status add_result = _AddRecord(internal_id, interned_vector->Str());
  if (!add_result.ok()) {
    auto untrack_result = UnTrackKey(key);
    if (!untrack_result.ok()) {
      VMSDK_LOG_EVERY_N_SEC(WARNING, nullptr, 1)
          << "While processing error for AddRecord, encountered error in "
             "UntrackKey: "
          << untrack_result.status().message();
    }
    return add_result;
  }
  return true;
}

absl::StatusOr<uint64_t> VectorBase::GetInternalId(
    const InternedStringPtr &key) const {
  absl::ReaderMutexLock lock(&key_to_metadata_mutex_);
  auto it = tracked_metadata_by_key_.find(key);
  if (it == tracked_metadata_by_key_.end()) {
    return absl::InvalidArgumentError("Record was not found");
  }
  return it->second.internal_id;
}

absl::StatusOr<uint64_t> VectorBase::GetInternalIdDuringSearch(
    const InternedStringPtr &key) const {
  auto it = tracked_metadata_by_key_.find(key);
  if (it == tracked_metadata_by_key_.end()) {
    return absl::InvalidArgumentError("Record was not found");
  }
  return it->second.internal_id;
}

absl::StatusOr<InternedStringPtr> VectorBase::GetKeyDuringSearch(
    uint64_t internal_id) const {
  auto it = key_by_internal_id_.find(internal_id);
  if (it == key_by_internal_id_.end()) {
    return absl::InvalidArgumentError("Record was not found");
  }
  return it->second;
}

absl::StatusOr<bool> VectorBase::ModifyRecord(const InternedStringPtr &key,
                                              absl::string_view record) {
  // VectorExternalizer tracks added entries. We need to untrack mutations which
  // are processed as modified records.
  std::optional<float> magnitude;
  auto interned_vector = InternVector(record, magnitude);
  if (!interned_vector) {
     [[maybe_unused]] auto res =
        RemoveRecord(key, indexes::DeletionType::kRecord);
    return false;
  }
  VMSDK_ASSIGN_OR_RETURN(auto internal_id, GetInternalId(key));
  VMSDK_RETURN_IF_ERROR(UpdateMetadata(
      key, magnitude.value_or(kDefaultMagnitude), interned_vector));
  auto modify_result = _ModifyRecord(internal_id, interned_vector->Str());
  if (!modify_result.ok()) {
    auto untrack_result = UnTrackKey(key);
    if (!untrack_result.ok()) {
      VMSDK_LOG_EVERY_N_SEC(WARNING, nullptr, 1)
          << "While processing error for ModifyRecord, encountered error "
             "in UntrackKey: "
          << untrack_result.status().message();
    }
  }
  return modify_result;
}

template <typename T>
absl::StatusOr<std::deque<Neighbor>> VectorBase::CreateReply(
    std::priority_queue<std::pair<T, hnswlib::labeltype>> &knn_res) {
  std::deque<Neighbor> ret;
  while (!knn_res.empty()) {
    auto &ele = knn_res.top();
    auto vector_key = GetKeyDuringSearch(ele.second);
    if (!vector_key.ok()) {
      knn_res.pop();
      continue;
    }
    ret.emplace_back(Neighbor{vector_key.value(), ele.first});
    knn_res.pop();
  }
  return ret;
}

absl::StatusOr<std::vector<char>> VectorBase::GetValue(
    const InternedStringPtr &key) const {
  auto it = tracked_metadata_by_key_.find(key);
  if (it == tracked_metadata_by_key_.end()) {
    return absl::NotFoundError("Record was not found");
  }
  std::vector<char> result;
  char *value = _GetValue(it->second.internal_id);
  if (normalize_) {
    if (it->second.magnitude < 0) {
      return absl::InternalError("Magnitude is not initialized");
    }
    result = DenormalizeVector(absl::string_view(value, GetVectorDataSize()),
                               GetDataTypeSize(), it->second.magnitude);
  } else {
    result.assign(value, value + GetVectorDataSize());
  }
  return result;
}

bool VectorBase::IsTracked(const InternedStringPtr &key) const {
  absl::ReaderMutexLock lock(&key_to_metadata_mutex_);
  auto it = tracked_metadata_by_key_.find(key);
  return (it != tracked_metadata_by_key_.end());
}

absl::StatusOr<bool> VectorBase::RemoveRecord(
    const InternedStringPtr &key,
    [[maybe_unused]] indexes::DeletionType deletion_type) {
  VMSDK_ASSIGN_OR_RETURN(auto res, UnTrackKey(key));
  if (!res.has_value()) {
    return false;
  }
  VMSDK_RETURN_IF_ERROR(_RemoveRecord(res.value()));
  return true;
}

absl::StatusOr<std::optional<uint64_t>> VectorBase::UnTrackKey(
    const InternedStringPtr &key) {
  if (key->Str().empty()) {
    return std::nullopt;
  }
  absl::WriterMutexLock lock(&key_to_metadata_mutex_);
  auto it = tracked_metadata_by_key_.find(key);
  if (it == tracked_metadata_by_key_.end()) {
    return std::nullopt;
  }
  auto id = it->second.internal_id;
  UnTrackVector(id);
  tracked_metadata_by_key_.erase(it);
  auto key_by_internal_id_it = key_by_internal_id_.find(id);
  if (key_by_internal_id_it == key_by_internal_id_.end()) {
    return absl::InvalidArgumentError(
        "Error while untracking key - key was not found in key_by_internal_id_ "
        "but in internal_by_key_");
  }
  key_by_internal_id_.erase(key_by_internal_id_it);
  return id;
}

char *VectorBase::TrackVector(uint64_t internal_id, char *vector, size_t len) {
  auto interned_vector = StringInternStore::Intern(
      absl::string_view(vector, len), vector_allocator_.get());
  return TrackVector(internal_id, interned_vector);
}

absl::StatusOr<uint64_t> VectorBase::TrackKey(const InternedStringPtr &key,
                                              float magnitude,
                                              const InternedStringPtr &vector) {
  if (key->Str().empty()) {
    return absl::InvalidArgumentError("key can't be empty");
  }
  absl::WriterMutexLock lock(&key_to_metadata_mutex_);
  auto id = inc_id_++;
  auto [_, succ] = tracked_metadata_by_key_.insert(
      {key, {.internal_id = id, .magnitude = magnitude}});
  TrackVector(id, vector);
  if (!succ) {
    return absl::InvalidArgumentError(
        absl::StrCat("Embedding id already exists: ", key->Str()));
  }
  key_by_internal_id_.insert({id, key});
  return id;
}

absl::Status VectorBase::UpdateMetadata(const InternedStringPtr &key,
                                        float magnitude,
                                        const InternedStringPtr &vector) {
  if (key->Str().empty()) {
    return absl::InvalidArgumentError("key can't be empty");
  }
  absl::WriterMutexLock lock(&key_to_metadata_mutex_);
  auto it = tracked_metadata_by_key_.find(key);
  if (it == tracked_metadata_by_key_.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Embedding id not found: ", key->Str()));
  }
  it->second.magnitude = magnitude;
  TrackVector(it->second.internal_id, vector);
  return absl::OkStatus();
}

int VectorBase::RespondWithInfo(RedisModuleCtx *ctx) const {
  RedisModule_ReplyWithSimpleString(ctx, "type");
  RedisModule_ReplyWithSimpleString(ctx, "VECTOR");
  RedisModule_ReplyWithSimpleString(ctx, "index");

  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
  RedisModule_ReplyWithSimpleString(ctx, "capacity");
  RedisModule_ReplyWithLongLong(ctx, GetCapacity());
  RedisModule_ReplyWithSimpleString(ctx, "dimensions");
  RedisModule_ReplyWithLongLong(ctx, dimensions_);
  RedisModule_ReplyWithSimpleString(ctx, "distance_metric");
  RedisModule_ReplyWithSimpleString(
      ctx, LookupKeyByValue(*kDistanceMetricByStr, distance_metric_).data());
  RedisModule_ReplyWithSimpleString(ctx, "size");
  {
    absl::MutexLock lock(&key_to_metadata_mutex_);
    RedisModule_ReplyWithCString(
        ctx, std::to_string(key_by_internal_id_.size()).c_str());
  }
  int array_len = 8;
  array_len += _RespondWithInfo(ctx);
  RedisModule_ReplySetArrayLength(ctx, array_len);

  return 4;
}

absl::Status VectorBase::SaveIndex(RDBOutputStream &rdb_stream) const {
  VMSDK_RETURN_IF_ERROR(_SaveIndex(rdb_stream));
  return absl::OkStatus();
}

void VectorBase::ExternalizeVector(RedisModuleCtx *ctx,
                                   const AttributeDataType *attribute_data_type,
                                   absl::string_view key_cstr,
                                   absl::string_view attribute_identifier) {
  auto key_obj = vmsdk::MakeUniqueRedisOpenKey(
      ctx, vmsdk::MakeUniqueRedisString(key_cstr).get(),
      REDISMODULE_OPEN_KEY_NOEFFECTS | REDISMODULE_READ);
  if (!key_obj || !attribute_data_type->IsProperType(key_obj.get())) {
    return;
  }
  bool is_module_owned;
  vmsdk::UniqueRedisString record = VectorExternalizer::Instance().GetRecord(
      ctx, attribute_data_type, key_obj.get(), key_cstr, attribute_identifier,
      is_module_owned);
  CHECK(!is_module_owned);
  std::optional<float> magnitude;
  auto interned_key = StringInternStore::Intern(key_cstr);
  auto interned_vector =
      InternVector(vmsdk::ToStringView(record.get()), magnitude);
  if (interned_vector) {
    VectorExternalizer::Instance().Externalize(
        interned_key, attribute_identifier, attribute_data_type->ToProto(),
        interned_vector, magnitude);
  }
}

absl::Status VectorBase::LoadTrackedKeys(
    RedisModuleCtx *ctx, const AttributeDataType *attribute_data_type,
    const data_model::TrackedKeys &tracked_keys) {
  absl::WriterMutexLock lock(&key_to_metadata_mutex_);
  for (const auto &tracked_key_metadata : tracked_keys.tracked_key_metadata()) {
    auto interned_key = StringInternStore::Intern(tracked_key_metadata.key());
    tracked_metadata_by_key_.insert(
        {interned_key,
         {.internal_id = tracked_key_metadata.internal_id(),
          .magnitude = tracked_key_metadata.magnitude()}});
    key_by_internal_id_.insert(
        {tracked_key_metadata.internal_id(), interned_key});
    inc_id_ = std::max(inc_id_, tracked_key_metadata.internal_id());
    ExternalizeVector(ctx, attribute_data_type, tracked_key_metadata.key(),
                      attribute_identifier_);
  }
  ++inc_id_;
  return absl::OkStatus();
}

absl::Status VectorBase::LoadKeysAndInternalIds(
    RedisModuleCtx *ctx, const AttributeDataType *attribute_data_type,
    RDBInputStream &rdb_stream) {
  absl::WriterMutexLock lock(&key_to_metadata_mutex_);
  size_t keys_count;
  VMSDK_RETURN_IF_ERROR(rdb_stream.loadSizeT(keys_count))
      << "Error loading keys count";
  for (size_t i = 0; i < keys_count; ++i) {
    size_t id;
    VMSDK_RETURN_IF_ERROR(rdb_stream.loadSizeT(id)) << "Error loading id";
    VMSDK_ASSIGN_OR_RETURN(auto key, rdb_stream.loadString(),
                           _ << "Error loading key");
    auto interned_key =
        StringInternStore::Intern(vmsdk::ToStringView(key.get()));
    key_by_internal_id_.insert({id, interned_key});
    tracked_metadata_by_key_.insert(
        {interned_key,
         {.internal_id = id,
          // Use negative infinity as a placeholder for magnitude. It will be
          // updated on backfill. In the meantime, we will need to fetch vector
          // contents from the main dictionary if specified in the query RETURN.
          .magnitude =
              normalize_ ? std::numeric_limits<float>::lowest() : -1.0f}});
    inc_id_ = std::max(inc_id_, id);
    ExternalizeVector(ctx, attribute_data_type, interned_key->Str(),
                      attribute_identifier_);
  }
  ++inc_id_;
  return absl::OkStatus();
}

std::unique_ptr<data_model::Index> VectorBase::ToProto() const {
  absl::ReaderMutexLock lock(&key_to_metadata_mutex_);
  auto index_proto = std::make_unique<data_model::Index>();
  auto vector_index = std::make_unique<data_model::VectorIndex>();
  vector_index->set_normalize(normalize_);
  vector_index->set_distance_metric(distance_metric_);
  vector_index->set_dimension_count(dimensions_);
  vector_index->set_initial_cap(GetCapacity());
  _ToProto(vector_index.get());
  vector_index->mutable_tracked_keys()->mutable_tracked_key_metadata()->Reserve(
      tracked_metadata_by_key_.size());
  for (const auto &[key, metadata] : tracked_metadata_by_key_) {
    auto tracked_key_metadata =
        vector_index->mutable_tracked_keys()->add_tracked_key_metadata();
    tracked_key_metadata->set_key(*key);
    tracked_key_metadata->set_internal_id(metadata.internal_id);
    tracked_key_metadata->set_magnitude(metadata.magnitude);
  }
  index_proto->set_allocated_vector_index(vector_index.release());
  return index_proto;
}

absl::StatusOr<std::pair<float, hnswlib::labeltype>>
VectorBase::ComputeDistanceFromRecord(const InternedStringPtr &key,
                                      absl::string_view query) const {
  VMSDK_ASSIGN_OR_RETURN(auto internal_id, GetInternalIdDuringSearch(key));
  return _ComputeDistanceFromRecord(internal_id, query);
}

void VectorBase::AddPrefilteredKey(
    absl::string_view query, uint64_t count, const InternedStringPtr &key,
    std::priority_queue<std::pair<float, hnswlib::labeltype>> &results,
    absl::flat_hash_set<hnswlib::labeltype> &top_keys) const {
  auto result = ComputeDistanceFromRecord(key, query);
  if (!result.ok() || top_keys.contains(result.value().second)) {
    return;
  }
  if (results.size() < count) {
    results.emplace(result.value());
    top_keys.insert(result.value().second);
  } else if (result.value().first < results.top().first) {
    auto top = results.top();
    top_keys.erase(top.second);
    results.pop();
    results.emplace(result.value());
    top_keys.insert(result.value().second);
  }
}

absl::string_view TrimBrackets(absl::string_view input) {
  if (absl::ConsumePrefix(&input, "[")) {
    absl::ConsumeSuffix(&input, "]");
    return TrimBrackets(input);
  }
  return input;
}

vmsdk::UniqueRedisString VectorBase::NormalizeStringRecord(
    vmsdk::UniqueRedisString input) const {
  CHECK_EQ(GetDataTypeSize(), sizeof(float));
  auto input_str = TrimBrackets(vmsdk::ToStringView(input.get()));
  std::vector<std::string> float_strings =
      absl::StrSplit(input_str, ',', absl::SkipWhitespace());
  std::string binary_string;
  binary_string.reserve(float_strings.size() * sizeof(float));
  for (const auto &float_str : float_strings) {
    float value;
    if (!absl::SimpleAtof(float_str, &value)) {
      return nullptr;
    }
    binary_string += std::string((char *)&value, sizeof(float));
  }
  return vmsdk::MakeUniqueRedisString(binary_string);
}

uint64_t VectorBase::GetRecordCount() const {
  absl::ReaderMutexLock lock(&key_to_metadata_mutex_);
  return key_by_internal_id_.size();
}

template void VectorBase::Init<float>(
    int dimensions, data_model::DistanceMetric distance_metric,
    std::unique_ptr<hnswlib::SpaceInterface<float>> &space);

template absl::StatusOr<std::deque<Neighbor>> VectorBase::CreateReply<float>(
    std::priority_queue<std::pair<float, hnswlib::labeltype>> &knn_res);
}  // namespace indexes

}  // namespace valkey_search
