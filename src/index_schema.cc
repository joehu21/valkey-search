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

#include "src/index_schema.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "src/attribute.h"
#include "src/attribute_data_type.h"
#include "src/index_schema.pb.h"
#include "src/indexes/index_base.h"
#include "src/indexes/numeric.h"
#include "src/indexes/tag.h"
#include "src/indexes/vector_base.h"
#include "src/indexes/vector_flat.h"
#include "src/indexes/vector_hnsw.h"
#include "src/keyspace_event_manager.h"
#include "src/metrics.h"
#include "src/rdb_io_stream.h"
#include "src/utils/string_interning.h"
#include "src/vector_externalizer.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/module_type.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/thread_pool.h"
#include "vmsdk/src/time_sliced_mrmw_mutex.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/utils.h"

namespace valkey_search {

constexpr int kEncodingVersion = 0;

LogLevel GetLogSeverity(bool ok) { return ok ? DEBUG : WARNING; }

std::string IndexSchema::GetRedisKeyForIndexSchemaName(absl::string_view name) {
  return absl::StrCat(kIndexSchemaKeyPrefix, name);
}

IndexSchema::BackfillJob::BackfillJob(RedisModuleCtx *ctx,
                                      absl::string_view name, int db_num)
    : cursor(vmsdk::MakeUniqueRedisScanCursor()) {
  scan_ctx = vmsdk::MakeUniqueRedisDetachedThreadSafeContext(ctx);
  RedisModule_SelectDb(scan_ctx.get(), db_num);
  db_size = RedisModule_DbSize(scan_ctx.get());
  VMSDK_LOG(NOTICE, ctx) << "Starting backfill for index schema in DB "
                         << db_num << ": " << name;
}

absl::StatusOr<std::shared_ptr<indexes::IndexBase>> IndexFactory(
    RedisModuleCtx *ctx, IndexSchema *index_schema,
    const data_model::Attribute &attribute,
    std::optional<RDBInputStream *> rdb_stream,
    data_model::AttributeDataType attribute_data_type) {
  const auto &index = attribute.index();
  switch (index.index_type_case()) {
    case data_model::Index::IndexTypeCase::kTagIndex: {
      return std::make_shared<indexes::Tag>(index.tag_index());
    }
    case data_model::Index::IndexTypeCase::kNumericIndex: {
      return std::make_shared<indexes::Numeric>(index.numeric_index());
    }
    case data_model::Index::IndexTypeCase::kVectorIndex: {
      switch (index.vector_index().algorithm_case()) {
        case data_model::VectorIndex::kHnswAlgorithm: {
          switch (index.vector_index().vector_data_type()) {
            case data_model::VECTOR_DATA_TYPE_FLOAT32: {
              // TODO(sumish): Create an empty index in case of an error
              // loading the index contents from RDB.
              VMSDK_ASSIGN_OR_RETURN(
                  auto index,
                  (rdb_stream.has_value())
                      ? indexes::VectorHNSW<float>::LoadFromRDB(
                            ctx, &index_schema->GetAttributeDataType(),
                            index.vector_index(), *rdb_stream.value(),
                            attribute.identifier())
                      : indexes::VectorHNSW<float>::Create(
                            index.vector_index(), attribute.identifier(),
                            attribute_data_type));
              index_schema->SubscribeToVectorExternalizer(
                  attribute.identifier(), index.get());
              return index;
            }
            default: {
              return absl::InvalidArgumentError(
                  "Unsupported vector data type.");
            }
          }
        }
        case data_model::VectorIndex::kFlatAlgorithm: {
          switch (index.vector_index().vector_data_type()) {
            case data_model::VECTOR_DATA_TYPE_FLOAT32: {
              // TODO(sumish): Create an empty index in case of an error
              // loading the index contents from RDB.
              VMSDK_ASSIGN_OR_RETURN(
                  auto index,
                  (rdb_stream.has_value())
                      ? indexes::VectorFlat<float>::LoadFromRDB(
                            ctx, &index_schema->GetAttributeDataType(),
                            index.vector_index(), *rdb_stream.value(),
                            attribute.identifier())
                      : indexes::VectorFlat<float>::Create(
                            index.vector_index(), attribute.identifier(),
                            attribute_data_type));
              index_schema->SubscribeToVectorExternalizer(
                  attribute.identifier(), index.get());
              return index;
            }
            default: {
              return absl::InvalidArgumentError(
                  "Unsupported vector data type.");
            }
          }
        }
        default: {
          return absl::InvalidArgumentError("Unsupported algorithm.");
        }
      }
      break;
    }
    default: {
      return absl::InvalidArgumentError("Unsupported index type.");
    }
  }
}

absl::StatusOr<std::shared_ptr<IndexSchema>> IndexSchema::Create(
    RedisModuleCtx *ctx, const data_model::IndexSchema &index_schema_proto,
    RedisModuleType *module_type, vmsdk::ThreadPool *mutations_thread_pool,
    std::unique_ptr<data_model::IndexSchema_Stats> stats) {
  std::unique_ptr<AttributeDataType> attribute_data_type;
  switch (index_schema_proto.attribute_data_type()) {
    case data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_HASH:
      attribute_data_type = std::make_unique<HashAttributeDataType>();
      break;
    case data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_JSON:
      if (!IsJsonModuleLoaded(ctx)) {
        return absl::InvalidArgumentError("JSON module is not loaded");
      }
      attribute_data_type = std::make_unique<JsonAttributeDataType>();
      break;
    default:
      return absl::InvalidArgumentError("Unsupported attribute data type.");
  }
  auto index_data_type = attribute_data_type->ToProto();

  auto res = std::shared_ptr<IndexSchema>(
      new IndexSchema(ctx, index_schema_proto, std::move(attribute_data_type),
                      module_type, mutations_thread_pool, std::move(stats)));
  VMSDK_RETURN_IF_ERROR(res->Init(ctx));
  for (const auto &attribute : index_schema_proto.attributes()) {
    VMSDK_ASSIGN_OR_RETURN(
        std::shared_ptr<indexes::IndexBase> index,
        IndexFactory(ctx, res.get(), attribute, std::nullopt, index_data_type));
    VMSDK_RETURN_IF_ERROR(
        res->AddIndex(attribute.alias(), attribute.identifier(), index));
  }
  return res;
}

vmsdk::MRMWMutexOptions CreateMrmwMutexOptions() {
  vmsdk::MRMWMutexOptions options;
  options.read_quota_duration = absl::Milliseconds(10);
  options.read_switch_grace_period = absl::Milliseconds(1);
  options.write_quota_duration = absl::Milliseconds(1);
  options.write_switch_grace_period = absl::Microseconds(200);
  return options;
}

IndexSchema::IndexSchema(RedisModuleCtx *ctx,
                         const data_model::IndexSchema &index_schema_proto,
                         std::unique_ptr<AttributeDataType> attribute_data_type,
                         RedisModuleType *module_type,
                         vmsdk::ThreadPool *mutations_thread_pool,
                         std::unique_ptr<data_model::IndexSchema_Stats> stats)
    : vmsdk::ModuleType(
          ctx, GetRedisKeyForIndexSchemaName(index_schema_proto.name()),
          module_type),
      keyspace_event_manager_(&KeyspaceEventManager::Instance()),
      attribute_data_type_(std::move(attribute_data_type)),
      name_(std::string(index_schema_proto.name())),
      db_num_(index_schema_proto.db_num()),
      mutations_thread_pool_(mutations_thread_pool),
      time_sliced_mutex_(CreateMrmwMutexOptions()) {
  RedisModule_SelectDb(detached_ctx_.get(), db_num_);
  if (index_schema_proto.subscribed_key_prefixes().empty()) {
    subscribed_key_prefixes_.push_back("");
    return;
  }
  for (const auto &key_prefix : index_schema_proto.subscribed_key_prefixes()) {
    if (!std::any_of(
            subscribed_key_prefixes_.begin(), subscribed_key_prefixes_.end(),
            [&](const std::string &s) { return key_prefix.starts_with(s); })) {
      subscribed_key_prefixes_.push_back(std::string(key_prefix));
    }
  }
  if (stats != nullptr) {
    stats_.document_cnt = stats->documents_count();
  }
}

absl::Status IndexSchema::Init(RedisModuleCtx *ctx) {
  VMSDK_RETURN_IF_ERROR(keyspace_event_manager_->InsertSubscription(ctx, this));
  backfill_job_ = std::make_optional<BackfillJob>(ctx, name_, db_num_);
  return absl::OkStatus();
}

IndexSchema::~IndexSchema() {
  VMSDK_LOG(NOTICE, detached_ctx_.get())
      << "Index schema " << name_ << " dropped from DB " << db_num_;

  // If we are not already destructing, make sure we perform necessary cleanup.
  // Note that this will fail on background threads, so indices should be marked
  // as destructing by the main thread.
  if (!is_destructing_) {
    MarkAsDestructing();
  }
}

namespace type_methods {

absl::Status RDBSaveImpl(RedisModuleIO *rdb, void *value) {
  if (rdb == nullptr) {
    DCHECK(false);
    return absl::InvalidArgumentError("RDBSave was called with nullptr as rdb");
  }

  if (value == nullptr) {
    return absl::InvalidArgumentError(
        "RDBSave was called with null value to save");
  }

  auto index_schema_ptr = (IndexSchema *)value;

  // Promoting code resiliency by getting the shared ptr
  auto index_schema = index_schema_ptr->GetSharedPtr();
  RDBOutputStream rdb_stream(rdb);
  return index_schema->RDBSave(rdb_stream);
}

void RDBSave(RedisModuleIO *rdb, void *value) {
  absl::Status status = RDBSaveImpl(rdb, value);
  if (!status.ok()) {
    Metrics::GetStats().rdb_save_failure_cnt++;
    VMSDK_IO_LOG_EVERY_N_SEC(WARNING, rdb, 0.1)
        << "Failed to save to RDB: " << status.ToString();
  }
  Metrics::GetStats().rdb_save_success_cnt++;
}

void AOFRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
  DCHECK(aof != nullptr);
  if (key == nullptr || value == nullptr) {
    VMSDK_IO_LOG_EVERY_N_SEC(WARNING, aof, 5)
        << "null key or value to rewrite aof";
    DCHECK(false);
    return;
  }
}

void Digest(RedisModuleDigest *md, void *value) {}
size_t MemUsage(const void *value) { return 1000; }
}  // namespace type_methods

absl::StatusOr<RedisModuleType *> IndexSchema::CreateModuleType(
    RedisModuleCtx *ctx, RDBLoadFunc rdb_load_func) {
  static RedisModuleTypeMethods tm = {
      .version = REDISMODULE_TYPE_METHOD_VERSION,
      .rdb_load = rdb_load_func,
      .rdb_save = type_methods::RDBSave,
      .aof_rewrite = type_methods::AOFRewrite,
      .mem_usage = type_methods::MemUsage,
      .digest = type_methods::Digest,
      // We no longer track the value in the keyspace, so free does nothing.
      .free = [](void *val) { /* do nothing */ },
      .aux_load = nullptr,
      .aux_save = nullptr,
      .aux_save_triggers = 0,
      .free_effort = nullptr,
      .unlink = nullptr,
      .copy = nullptr,
      .defrag = nullptr,
      .mem_usage2 = nullptr,
      .free_effort2 = nullptr,
      .unlink2 = nullptr,
      .copy2 = nullptr};

  auto module_type = RedisModule_CreateDataType(
      ctx, kIndexSchemaModuleTypeName.data(), kEncodingVersion, &tm);
  if (!module_type) {
    return absl::InternalError(
        absl::StrCat("failed to create ", kIndexSchemaModuleTypeName, " type"));
  }
  return module_type;
}

absl::StatusOr<std::shared_ptr<indexes::IndexBase>> IndexSchema::GetIndex(
    absl::string_view attribute_alias) const {
  auto itr = attributes_.find(std::string{attribute_alias});
  if (ABSL_PREDICT_FALSE(itr == attributes_.end())) {
    return absl::NotFoundError(
        absl::StrCat("Index field `", attribute_alias, "` not exists"));
  }
  return itr->second.GetIndex();
}

absl::StatusOr<std::string> IndexSchema::GetIdentifier(
    absl::string_view attribute_alias) const {
  auto itr = attributes_.find(std::string{attribute_alias});
  if (itr == attributes_.end()) {
    return absl::NotFoundError("Index field not exists");
  }
  return itr->second.GetIdentifier();
}

absl::StatusOr<vmsdk::UniqueRedisString> IndexSchema::DefaultReplyScoreAs(
    absl::string_view attribute_alias) const {
  auto itr = attributes_.find(std::string{attribute_alias});
  if (ABSL_PREDICT_FALSE(itr == attributes_.end())) {
    return absl::NotFoundError(
        absl::StrCat("Index field `", attribute_alias, "` not exists"));
  }
  return itr->second.DefaultReplyScoreAs();
}

absl::Status IndexSchema::AddIndex(absl::string_view attribute_alias,
                                   absl::string_view identifier,
                                   std::shared_ptr<indexes::IndexBase> index) {
  auto [_, res] =
      attributes_.insert({std::string(attribute_alias),
                          Attribute{attribute_alias, identifier, index}});
  if (!res) {
    return absl::AlreadyExistsError("Index field already exists");
  }
  return absl::OkStatus();
}

void TrackResults(
    RedisModuleCtx *ctx, const absl::StatusOr<bool> &status_or,
    const char *operation_str,
    IndexSchema::Stats::ResultCnt<std::atomic<uint64_t>> &counter) {
  if (ABSL_PREDICT_FALSE(!status_or.ok())) {
    ++counter.failure_cnt;
  } else if (status_or.value()) {
    ++counter.success_cnt;
  } else {
    ++counter.skipped_cnt;
  }
  // Separate errors and successes so that they log on different timers.
  if (ABSL_PREDICT_TRUE(status_or.ok())) {
    VMSDK_LOG_EVERY_N_SEC(GetLogSeverity(status_or.ok()), ctx, 5)
        << operation_str
        << " succeeded with result: " << status_or.status().ToString();
  } else {
    VMSDK_LOG_EVERY_N_SEC(GetLogSeverity(status_or.ok()), ctx, 1)
        << operation_str
        << " failed with result: " << status_or.status().ToString();
  }
}

void IndexSchema::OnKeyspaceNotification(RedisModuleCtx *ctx, int type,
                                         const char *event,
                                         RedisModuleString *key) {
  if (ABSL_PREDICT_FALSE(!IsInCurrentDB(ctx))) {
    return;
  }
  ProcessKeyspaceNotification(ctx, key, false);
}

bool AddAttributeData(IndexSchema::MutatedAttributes &mutated_attributes,
                      const Attribute &attribute,
                      AttributeDataType &attribute_data_type,
                      vmsdk::UniqueRedisString record) {
  if (record) {
    if (attribute_data_type.RecordsProvidedAsString()) {
      auto normalized_record =
          attribute.GetIndex()->NormalizeStringRecord(std::move(record));
      if (!normalized_record) {
        return false;
      }
      mutated_attributes[attribute.GetAlias()].data =
          std::move(normalized_record);
    } else {
      mutated_attributes[attribute.GetAlias()].data = std::move(record);
    }
  } else {
    mutated_attributes[attribute.GetAlias()].data = nullptr;
  }
  return true;
}

void IndexSchema::ProcessKeyspaceNotification(RedisModuleCtx *ctx,
                                              RedisModuleString *key,
                                              bool from_backfill) {
  auto key_cstr = vmsdk::ToStringView(key);
  if (key_cstr.empty()) {
    return;
  }
  auto key_obj = vmsdk::MakeUniqueRedisOpenKey(
      ctx, key, REDISMODULE_OPEN_KEY_NOEFFECTS | REDISMODULE_READ);
  // Fail fast if the key type does not match the data type.
  if (key_obj && !GetAttributeDataType().IsProperType(key_obj.get())) {
    return;
  }
  MutatedAttributes mutated_attributes;
  bool added = false;
  auto interned_key = StringInternStore::Intern(key_cstr);
  for (const auto &attribute_itr : attributes_) {
    auto &attribute = attribute_itr.second;
    if (!key_obj) {
      added = true;
      mutated_attributes[attribute_itr.first] = {
          nullptr, indexes::DeletionType::kRecord};
      continue;
    }
    bool is_module_owned;
    vmsdk::UniqueRedisString record = VectorExternalizer::Instance().GetRecord(
        ctx, attribute_data_type_.get(), key_obj.get(), key_cstr,
        attribute.GetIdentifier(), is_module_owned);
    if (!is_module_owned) {
      // A record which are owned by the module were not modified and are
      // already tracked in the vector registry.
      VectorExternalizer(interned_key, attribute.GetIdentifier(), record);
    }
    if (AddAttributeData(mutated_attributes, attribute, *attribute_data_type_,
                         std::move(record))) {
      added = true;
    }
  }
  if (added) {
    ProcessMutation(ctx, mutated_attributes, interned_key, from_backfill);
  }
}

bool IndexSchema::IsTrackedByAnyIndex(const InternedStringPtr &key) const {
  return std::any_of(attributes_.begin(), attributes_.end(),
                     [&key](const auto &attribute) {
                       return attribute.second.GetIndex()->IsTracked(key);
                     });
}

void IndexSchema::SyncProcessMutation(RedisModuleCtx *ctx,
                                      MutatedAttributes &mutated_attributes,
                                      const InternedStringPtr &key) {
  vmsdk::WriterMutexLock lock(&time_sliced_mutex_);
  for (auto &attribute_data_itr : mutated_attributes) {
    const auto itr = attributes_.find(attribute_data_itr.first);
    if (itr == attributes_.end()) {
      continue;
    }
    ProcessAttributeMutation(ctx, itr->second, key,
                             std::move(attribute_data_itr.second.data),
                             attribute_data_itr.second.deletion_type);
  }
}

void IndexSchema::ProcessAttributeMutation(
    RedisModuleCtx *ctx, const Attribute &attribute,
    const InternedStringPtr &key, vmsdk::UniqueRedisString data,
    indexes::DeletionType deletion_type) {
  auto index = attribute.GetIndex();
  if (data) {
    DCHECK(deletion_type == indexes::DeletionType::kNone);
    auto data_view = vmsdk::ToStringView(data.get());
    if (index->IsTracked(key)) {
      auto res = index->ModifyRecord(key, data_view);
      TrackResults(ctx, res, "Modify", stats_.subscription_modify);
      return;
    }
    bool was_tracked = IsTrackedByAnyIndex(key);
    auto res = index->AddRecord(key, data_view);
    TrackResults(ctx, res, "Add", stats_.subscription_add);

    if (res.ok() && res.value()) {
      // Increment the hash key count if it wasn't tracked and we successfully
      // added it to the index.
      if (!was_tracked) {
        ++stats_.document_cnt;
      }
    }
    return;
  }

  auto res = index->RemoveRecord(key, deletion_type);
  TrackResults(ctx, res, "Remove", stats_.subscription_remove);
  if (res.ok() && res.value()) {
    // Reduce the hash key count if nothing is tracking the key anymore.
    if (!IsTrackedByAnyIndex(key)) {
      --stats_.document_cnt;
    }
  }
}

std::unique_ptr<vmsdk::StopWatch> CreateQueueDelayCapturer() {
  std::unique_ptr<vmsdk::StopWatch> ret;
  thread_local int cnt{0};
  ++cnt;
  if (ABSL_PREDICT_FALSE(cnt % 1000 == 0)) {
    ret = std::make_unique<vmsdk::StopWatch>();
  }
  return ret;
}

// ProcessMultiQueue is used to flush pending mutations occurring in a
// multi/exec transaction. This function is called lazily on the next FT.SEARCH
// command.
void IndexSchema::ProcessMultiQueue() {
  schedule_multi_exec_processing_ = false;
  auto &multi_mutations = multi_mutations_.Get();
  if (ABSL_PREDICT_TRUE(multi_mutations.keys.empty())) {
    return;
  }
  multi_mutations.blocking_counter =
      std::make_unique<absl::BlockingCounter>(multi_mutations.keys.size());
  vmsdk::WriterMutexLock lock(&time_sliced_mutex_);
  while (!multi_mutations.keys.empty()) {
    auto key = multi_mutations.keys.front();
    multi_mutations.keys.pop();
    ScheduleMutation(false, key, vmsdk::ThreadPool::Priority::kMax,
                     multi_mutations.blocking_counter.get());
  }
  multi_mutations.blocking_counter->Wait();
  multi_mutations.blocking_counter.reset();
}

void IndexSchema::EnqueueMultiMutation(const InternedStringPtr &key) {
  auto &multi_mutations = multi_mutations_.Get();
  multi_mutations.keys.push(key);
  if (multi_mutations.keys.size() >= mutations_thread_pool_->Size() &&
      !schedule_multi_exec_processing_.Get()) {
    schedule_multi_exec_processing_.Get() = true;
    vmsdk::RunByMain(
        [weak_index_schema = GetWeakPtr()]() mutable {
          auto index_schema = weak_index_schema.lock();
          if (!index_schema) {
            return;
          }
          index_schema->ProcessMultiQueue();
        },
        true);
  }
}

void IndexSchema::ScheduleMutation(bool from_backfill,
                                   const InternedStringPtr &key,
                                   vmsdk::ThreadPool::Priority priority,
                                   absl::BlockingCounter *blocking_counter) {
  {
    absl::MutexLock lock(&stats_.mutex_);
    ++stats_.mutation_queue_size_;
    if (ABSL_PREDICT_FALSE(from_backfill)) {
      ++stats_.backfill_inqueue_tasks;
    }
  }
  mutations_thread_pool_->Schedule(
      [from_backfill, weak_index_schema = GetWeakPtr(),
       ctx = detached_ctx_.get(), delay_capturer = CreateQueueDelayCapturer(),
       key_str = std::move(key), blocking_counter]() mutable {
        auto index_schema = weak_index_schema.lock();
        if (ABSL_PREDICT_FALSE(!index_schema)) {
          return;
        }
        index_schema->ProcessSingleMutationAsync(ctx, from_backfill, key_str,
                                                 delay_capturer.get());
        if (ABSL_PREDICT_FALSE(blocking_counter)) {
          blocking_counter->DecrementCount();
        }
      },
      priority);
}

inline bool ShouldBlockClient([[maybe_unused]] bool inside_multi_exec,
                              [[maybe_unused]] bool from_backfill) {
#ifdef BLOCK_CLIENT_ON_MUTATION
  return !inside_multi_exec && !from_backfill;
#else
  return false;
#endif
}

void IndexSchema::ProcessMutation(RedisModuleCtx *ctx,
                                  MutatedAttributes &mutated_attributes,
                                  const InternedStringPtr &interned_key,
                                  bool from_backfill) {
  if (ABSL_PREDICT_FALSE(!mutations_thread_pool_ ||
                         mutations_thread_pool_->Size() == 0)) {
    SyncProcessMutation(ctx, mutated_attributes, interned_key);
    return;
  }
  const bool inside_multi_exec =
      (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_MULTI) != 0;
  if (ABSL_PREDICT_FALSE(inside_multi_exec)) {
    EnqueueMultiMutation(interned_key);
  }
  const bool block_client = ShouldBlockClient(inside_multi_exec, from_backfill);
  if (ABSL_PREDICT_FALSE(!TrackMutatedRecord(ctx, interned_key,
                                             std::move(mutated_attributes),
                                             from_backfill, block_client)) ||
      inside_multi_exec) {
    // Skip scheduling if the mutation key has already been tracked or is part
    // of a multi exec command.
    return;
  }
  const vmsdk::ThreadPool::Priority priority =
      from_backfill ? vmsdk::ThreadPool::Priority::kLow
                    : vmsdk::ThreadPool::Priority::kHigh;
  ScheduleMutation(from_backfill, interned_key, priority, nullptr);
}

void IndexSchema::ProcessSingleMutationAsync(RedisModuleCtx *ctx,
                                             bool from_backfill,
                                             const InternedStringPtr &key,
                                             vmsdk::StopWatch *delay_capturer) {
  bool first_time = true;
  do {
    auto mutation_record = ConsumeTrackedMutatedAttribute(key, first_time);
    first_time = false;
    if (!mutation_record.has_value()) {
      break;
    }
    SyncProcessMutation(ctx, mutation_record.value(), key);
  } while (true);
  absl::MutexLock lock(&stats_.mutex_);
  --stats_.mutation_queue_size_;
  if (ABSL_PREDICT_FALSE(from_backfill)) {
    --stats_.backfill_inqueue_tasks;
  }
  if (ABSL_PREDICT_FALSE(delay_capturer)) {
    stats_.mutations_queue_delay_ = delay_capturer->Duration();
  }
}

void IndexSchema::BackfillScanCallback(RedisModuleCtx *ctx,
                                       RedisModuleString *keyname,
                                       RedisModuleKey *key, void *privdata) {
  IndexSchema *index_schema = reinterpret_cast<IndexSchema *>(privdata);
  index_schema->backfill_job_.Get()->scanned_key_count++;
  auto key_prefixes = index_schema->GetKeyPrefixes();
  auto key_cstr = vmsdk::ToStringView(keyname);
  if (std::any_of(key_prefixes.begin(), key_prefixes.end(),
                  [&key_cstr](const auto &key_prefix) {
                    return key_cstr.starts_with(key_prefix);
                  })) {
    index_schema->ProcessKeyspaceNotification(ctx, keyname, true);
  }
}

uint32_t IndexSchema::PerformBackfill(RedisModuleCtx *ctx,
                                      uint32_t batch_size) {
  auto &backfill_job = backfill_job_.Get();
  if (!backfill_job.has_value() || backfill_job->IsScanDone()) {
    return 0;
  }

  // We need to ensure the DB size is monotonically increasing, since it could
  // change during the backfill, in which case we may show incorrect progress.
  backfill_job->db_size =
      std::max(backfill_job->db_size,
               (uint64_t)RedisModule_DbSize(backfill_job->scan_ctx.get()));

  uint64_t start_scan_count = backfill_job->scanned_key_count;
  uint64_t &current_scan_count = backfill_job->scanned_key_count;
  while (current_scan_count - start_scan_count < batch_size) {
    // Scan will return zero if there are no more keys to scan. This could be
    // the case either if there are no keys at all or if we have reached the
    // end of the current iteration. Because of this, we use the scanned key
    // count to know how many keys we have scanned in total (either zero or
    // one).
    if (!RedisModule_Scan(backfill_job->scan_ctx.get(),
                          backfill_job->cursor.get(), BackfillScanCallback,
                          (void *)this)) {
      VMSDK_LOG(NOTICE, ctx)
          << "Index schema " << name_ << " finished backfill. Scanned "
          << backfill_job->scanned_key_count << " keys in "
          << absl::FormatDuration(backfill_job->stopwatch.Duration());
      uint32_t res = current_scan_count - start_scan_count;
      backfill_job->MarkScanAsDone();
      return res;
    }
  }
  return current_scan_count - start_scan_count;
}

float IndexSchema::GetBackfillPercent() const {
  const auto &backfill_job = backfill_job_.Get();
  if (!IsBackfillInProgress() || (backfill_job->db_size == 0)) {
    return 1;
  }
  DCHECK(backfill_job->scanned_key_count >= stats_.backfill_inqueue_tasks);
  auto processed_keys =
      backfill_job->scanned_key_count - stats_.backfill_inqueue_tasks;
  if (backfill_job->scanned_key_count < stats_.backfill_inqueue_tasks ||
      backfill_job->db_size < processed_keys) {
    // This is a special case. Our scanned key count could be bigger than the
    // DB size if we have resized the hash table during the scan, causing us
    // to reiterate over keys we have already processed. The number of keys
    // double counted should be relatively small. Because of this, we report
    // very close to 100% indicate we are almost done. We shouldn't be in this
    // state for long.
    return 0.99;
  }
  return (float)processed_keys / backfill_job->db_size;
}

uint64_t IndexSchema::CountRecords() const {
  uint64_t record_cnt = 0;
  for (const auto &attribute : attributes_) {
    record_cnt += attribute.second.GetIndex()->GetRecordCount();
  }
  return record_cnt;
}

void IndexSchema::RespondWithInfo(RedisModuleCtx *ctx) const {
  RedisModule_ReplyWithArray(ctx, 24);
  RedisModule_ReplyWithSimpleString(ctx, "index_name");
  RedisModule_ReplyWithSimpleString(ctx, name_.data());
  RedisModule_ReplyWithSimpleString(ctx, "index_options");
  RedisModule_ReplyWithArray(ctx, 0);

  RedisModule_ReplyWithSimpleString(ctx, "index_definition");
  RedisModule_ReplyWithArray(ctx, 6);
  RedisModule_ReplyWithSimpleString(ctx, "key_type");
  RedisModule_ReplyWithSimpleString(ctx,
                                    attribute_data_type_->ToString().c_str());
  RedisModule_ReplyWithSimpleString(ctx, "prefixes");
  RedisModule_ReplyWithArray(ctx, subscribed_key_prefixes_.size());
  for (const auto &prefix : subscribed_key_prefixes_) {
    RedisModule_ReplyWithSimpleString(ctx, prefix.c_str());
  }
  // hard-code default score of 1 as it's the only value we currently
  // supported.
  RedisModule_ReplyWithSimpleString(ctx, "default_score");
  RedisModule_ReplyWithCString(ctx, "1");

  RedisModule_ReplyWithSimpleString(ctx, "attributes");
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
  int attribute_array_len = 0;
  for (const auto &attribute : attributes_) {
    attribute_array_len += attribute.second.RespondWithInfo(ctx);
  }
  RedisModule_ReplySetArrayLength(ctx, attribute_array_len);

  RedisModule_ReplyWithSimpleString(ctx, "num_docs");
  RedisModule_ReplyWithCString(ctx,
                               std::to_string(stats_.document_cnt).c_str());
  // hard-code num_terms to 0 as it's related to fulltext indexes:
  // https://screenshot.googleplex.com/ARnMzcxVyqWfP6r
  RedisModule_ReplyWithSimpleString(ctx, "num_terms");
  RedisModule_ReplyWithCString(ctx, "0");
  RedisModule_ReplyWithSimpleString(ctx, "num_records");
  RedisModule_ReplyWithCString(ctx, std::to_string(CountRecords()).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "hash_indexing_failures");
  RedisModule_ReplyWithCString(
      ctx, absl::StrFormat("%lu", stats_.subscription_add.skipped_cnt).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "backfill_in_progress");
  RedisModule_ReplyWithCString(
      ctx, absl::StrFormat("%d", IsBackfillInProgress() ? 1 : 0).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "backfill_complete_percent");
  RedisModule_ReplyWithCString(
      ctx, absl::StrFormat("%f", GetBackfillPercent()).c_str());

  absl::MutexLock lock(&stats_.mutex_);
  RedisModule_ReplyWithSimpleString(ctx, "mutation_queue_size");
  RedisModule_ReplyWithCString(
      ctx, absl::StrFormat("%lu", stats_.mutation_queue_size_).c_str());
  RedisModule_ReplyWithSimpleString(ctx, "recent_mutations_queue_delay");
  RedisModule_ReplyWithCString(
      ctx, absl::StrFormat("%lu sec", (stats_.mutation_queue_size_ > 0
                                           ? stats_.mutations_queue_delay_ /
                                                 absl::Seconds(1)
                                           : 0))
               .c_str());
}

std::unique_ptr<data_model::IndexSchema> IndexSchema::ToProto() const {
  auto index_schema_proto = std::make_unique<data_model::IndexSchema>();
  index_schema_proto->set_name(this->name_);
  index_schema_proto->set_db_num(db_num_);
  index_schema_proto->mutable_subscribed_key_prefixes()->Add(
      subscribed_key_prefixes_.begin(), subscribed_key_prefixes_.end());
  index_schema_proto->set_attribute_data_type(attribute_data_type_->ToProto());
  auto stats = index_schema_proto->mutable_stats();
  stats->set_documents_count(stats_.document_cnt);
  std::transform(
      attributes_.begin(), attributes_.end(),
      google::protobuf::RepeatedPtrFieldBackInserter(
          index_schema_proto->mutable_attributes()),
      [](const auto &attribute) { return *attribute.second.ToProto(); });
  return index_schema_proto;
}

absl::Status IndexSchema::RDBSave(RDBOutputStream &rdb_stream) const {
  auto index_schema_proto = ToProto();
  auto index_schema_proto_str = index_schema_proto->SerializeAsString();
  auto index_schema_name = index_schema_proto->name().data();
  VMSDK_RETURN_IF_ERROR(rdb_stream.SaveStringBuffer(
      index_schema_proto_str.data(), index_schema_proto_str.size()))
      << "IO error while saving IndexSchema name: " << index_schema_name
      << " to RDB";
  VMSDK_RETURN_IF_ERROR(rdb_stream.SaveUnsigned(GetAttributeCount()))
      << "IO error while saving attribute count for IndexSchema (name: "
      << index_schema_name << " to RDB";

  for (auto &attribute : attributes_) {
    // Note that the serialised attribute proto is also stored as part of the
    // serialised index schema proto above. We store here again to avoid any
    // dependencies on the ordering of multiple attributes.
    // We could remove the duplication in the future.
    auto attribute_alias = attribute.first.data();
    auto attribute_proto = attribute.second.ToProto();
    auto attribute_str = attribute_proto->SerializeAsString();
    VMSDK_RETURN_IF_ERROR(
        rdb_stream.SaveStringBuffer(attribute_str.data(), attribute_str.size()))
        << "IO error while saving attribute metadata index name: "
        << index_schema_name << " attribute: " << attribute_alias << " to RDB";
    VMSDK_RETURN_IF_ERROR(attribute.second.GetIndex()->SaveIndex(rdb_stream))
        << "IO error while saving Index contents (index name: "
        << index_schema_name << ", attribute: " << attribute_alias
        << ") to RDB";
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<IndexSchema>> IndexSchema::LoadFromRDB(
    RDBInputStream &rdb_stream, RedisModuleCtx *ctx,
    RedisModuleType *module_type, vmsdk::ThreadPool *mutations_thread_pool,
    const int encoding_version) {
  if (encoding_version != kEncodingVersion) {
    return absl::InvalidArgumentError("Unsupported encoding version.");
  }
  VMSDK_ASSIGN_OR_RETURN(
      auto serialized_index_schema, rdb_stream.LoadString(),
      _ << "IO error while reading serialized IndexSchema from RDB");
  auto index_schema_proto = std::make_unique<data_model::IndexSchema>();
  if (!index_schema_proto->ParseFromString(
          std::string(vmsdk::ToStringView(serialized_index_schema.get())))) {
    return absl::InvalidArgumentError(
        "Failed to deserialize index schema read from RDB");
  }

  // TODO(b/332974693): This is temporary logic added to allow upgrading from
  // a prior RDB format to the new RDB format. Remove this logic after the
  // migration. The stats field was added only with the new RDB format. All
  // RDBs generated with the new format will have the stats field populated.
  bool load_index_contents = index_schema_proto->has_stats();

  auto stats = (load_index_contents)
                   ? std::make_unique<data_model::IndexSchema_Stats>(
                         index_schema_proto->stats())
                   : nullptr;
  if (load_index_contents) {
    // Clear the attributes, we will load these from the RDB afterwards
    // with the data from the RDB.
    index_schema_proto->clear_attributes();
  }
  if (!index_schema_proto->has_db_num()) {
    // Prior to setting the DB number, we inferred this based on the context.
    // TODO(b/349436336) Remove after rollout
    index_schema_proto->set_db_num(RedisModule_GetSelectedDb(ctx));
  }

  VMSDK_ASSIGN_OR_RETURN(
      auto index_schema,
      IndexSchema::Create(ctx, *index_schema_proto, module_type,
                          mutations_thread_pool, std::move(stats)));

  if (load_index_contents) {
    unsigned int attributes_count;
    VMSDK_RETURN_IF_ERROR(rdb_stream.LoadUnsigned(attributes_count))
        << "IO error while reading attributes count from RDB";

    for (size_t i = 0; i < attributes_count; ++i) {
      VMSDK_ASSIGN_OR_RETURN(
          auto serialized_attribute, rdb_stream.LoadString(),
          _ << "IO error while reading attribute metadata from RDB");
      auto attribute = std::make_unique<data_model::Attribute>();
      if (!attribute->ParseFromString(
              std::string(vmsdk::ToStringView(serialized_attribute.get())))) {
        return absl::InvalidArgumentError(
            "Failed to deserialize attribute metadata read from RDB");
      }
      VMSDK_ASSIGN_OR_RETURN(
          std::shared_ptr<indexes::IndexBase> index,
          IndexFactory(ctx, index_schema.get(), *attribute, &rdb_stream,
                       index_schema->GetAttributeDataType().ToProto()));
      VMSDK_RETURN_IF_ERROR(index_schema->AddIndex(
          attribute->alias(), attribute->identifier(), index));
    }
  }

  return index_schema;
}

bool IndexSchema::IsInCurrentDB(RedisModuleCtx *ctx) const {
  return RedisModule_GetSelectedDb(ctx) == db_num_;
}

void IndexSchema::OnSwapDB(RedisModuleSwapDbInfo *swap_db_info) {
  uint32_t curr_db = db_num_;
  uint32_t db_to_swap_to;
  if (curr_db == swap_db_info->dbnum_first) {
    db_to_swap_to = swap_db_info->dbnum_second;
  } else if (curr_db == swap_db_info->dbnum_second) {
    db_to_swap_to = swap_db_info->dbnum_first;
  } else {
    return;
  }
  db_num_ = db_to_swap_to;
  auto &backfill_job = backfill_job_.Get();
  if (IsBackfillInProgress() && !backfill_job->IsScanDone()) {
    RedisModule_SelectDb(backfill_job->scan_ctx.get(), db_to_swap_to);
  }
}

void IndexSchema::OnLoadingEnded(RedisModuleCtx *ctx) {
  // Clean up any potentially stale index entries that can arise from pending
  // record deletions being lost during RDB save.
  vmsdk::StopWatch stop_watch;
  RedisModule_SelectDb(ctx, db_num_);  // Make sure we are in the right DB.
  absl::flat_hash_map<std::string, MutatedAttributes> deletion_attributes;
  for (const auto &attribute : attributes_) {
    const auto &index = attribute.second.GetIndex();
    std::vector<std::string> to_delete;
    uint64_t key_size = 0;
    uint64_t stale_entries = 0;
    index->ForEachTrackedKey([ctx, &deletion_attributes, &key_size, &attribute,
                              &stale_entries](const InternedStringPtr &key) {
      auto r_str = vmsdk::MakeUniqueRedisString(*key);
      if (!RedisModule_KeyExists(ctx, r_str.get())) {
        deletion_attributes[std::string(*key)][attribute.second.GetAlias()] = {
            nullptr, indexes::DeletionType::kRecord};
        stale_entries++;
      }
      key_size++;
    });
    VMSDK_LOG(NOTICE, ctx) << "Deleting " << stale_entries
                           << " stale entries of " << key_size
                           << " total keys for " << "{Index: " << name_
                           << ", Attribute: " << attribute.first << "}";
  }
  VMSDK_LOG(NOTICE, ctx) << "Deleting " << deletion_attributes.size()
                         << " stale entries for " << "{Index: " << name_ << "}";

  for (auto &[key, attributes] : deletion_attributes) {
    auto interned_key = std::make_shared<InternedString>(key);
    ProcessMutation(ctx, attributes, interned_key, true);
  }
  VMSDK_LOG(NOTICE, ctx) << "Scanned index schema " << name_
                         << " for stale entries in "
                         << absl::FormatDuration(stop_watch.Duration());
}

// Returns true if the inserted key not exists otherwise false
bool IndexSchema::TrackMutatedRecord(RedisModuleCtx *ctx,
                                     const InternedStringPtr &key,
                                     MutatedAttributes &&mutated_attributes,
                                     bool from_backfill, bool block_client) {
  absl::MutexLock lock(&mutated_records_mutex_);
  auto [itr, inserted] =
      tracked_mutated_records_.insert({key, DocumentMutation{}});
  if (ABSL_PREDICT_TRUE(inserted)) {
    itr->second.attributes = MutatedAttributes();
    itr->second.attributes.value() = std::move(mutated_attributes);
    itr->second.from_backfill = from_backfill;
    if (ABSL_PREDICT_TRUE(block_client)) {
      vmsdk::BlockedClient blocked_client(ctx);
      blocked_client.MeasureTimeStart();
      itr->second.blocked_clients.emplace_back(std::move(blocked_client));
    }
    return true;
  }
  if (!itr->second.attributes.has_value()) {
    itr->second.attributes = MutatedAttributes();
  }
  for (auto &mutated_attribute : mutated_attributes) {
    itr->second.attributes.value()[mutated_attribute.first] =
        std::move(mutated_attribute.second);
  }
  if (ABSL_PREDICT_TRUE(block_client)) {
    vmsdk::BlockedClient blocked_client(ctx);
    blocked_client.MeasureTimeStart();
    itr->second.blocked_clients.emplace_back(std::move(blocked_client));
  }
  if (ABSL_PREDICT_FALSE(!from_backfill && itr->second.from_backfill)) {
    itr->second.from_backfill = false;
    return true;
  }
  return false;
}

void IndexSchema::MarkAsDestructing() {
  absl::MutexLock lock(&mutated_records_mutex_);
  auto status = keyspace_event_manager_->RemoveSubscription(this);
  if (!status.ok()) {
    VMSDK_LOG(WARNING, detached_ctx_.get())
        << "Failed to remove keyspace event subscription for index "
           "schema "
        << name_ << ": " << status.message();
  }
  backfill_job_.Get()->MarkScanAsDone();
  tracked_mutated_records_.clear();
  is_destructing_ = true;
}

std::optional<IndexSchema::MutatedAttributes>
IndexSchema::ConsumeTrackedMutatedAttribute(const InternedStringPtr &key,
                                            bool first_time) {
  absl::MutexLock lock(&mutated_records_mutex_);
  auto itr = tracked_mutated_records_.find(key);
  if (ABSL_PREDICT_FALSE(itr == tracked_mutated_records_.end())) {
    return std::nullopt;
  }
  if (ABSL_PREDICT_FALSE(first_time && itr->second.consume_in_progress)) {
    return std::nullopt;
  }
  itr->second.consume_in_progress = true;
  // Delete this tracked document if no additional mutations were tracked
  if (!itr->second.attributes.has_value()) {
    tracked_mutated_records_.erase(itr);
    return std::nullopt;
  }
  // Track entry is now first consumed
  auto mutated_attributes = std::move(itr->second.attributes.value());
  itr->second.attributes = std::nullopt;
  return mutated_attributes;
}

size_t IndexSchema::GetMutatedRecordsSize() const {
  absl::MutexLock lock(&mutated_records_mutex_);
  return tracked_mutated_records_.size();
}

void IndexSchema::SubscribeToVectorExternalizer(
    absl::string_view attribute_identifier, indexes::VectorBase *vector_index) {
  vector_externalizer_subscriptions_[attribute_identifier] = vector_index;
}

void IndexSchema::VectorExternalizer(const InternedStringPtr &key,
                                     absl::string_view attribute_identifier,
                                     vmsdk::UniqueRedisString &record) {
  auto it = vector_externalizer_subscriptions_.find(attribute_identifier);
  if (it == vector_externalizer_subscriptions_.end()) {
    return;
  }
  if (record) {
    std::optional<float> magnitude;
    auto vector_str = vmsdk::ToStringView(record.get());
    InternedStringPtr interned_vector =
        it->second->InternVector(vector_str, magnitude);
    if (interned_vector) {
      VectorExternalizer::Instance().Externalize(
          key, attribute_identifier, attribute_data_type_->ToProto(),
          interned_vector, magnitude);
    }
    return;
  }
  VectorExternalizer::Instance().Remove(key, attribute_identifier,
                                        attribute_data_type_->ToProto());
}

}  // namespace valkey_search
