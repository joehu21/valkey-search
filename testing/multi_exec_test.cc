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

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "src/commands/commands.h"
#include "src/utils/string_interning.h"
#include "src/valkey_search.h"
#include "testing/common.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"
#include "vmsdk/src/testing_infra/module.h"
#include "vmsdk/src/thread_pool.h"
namespace valkey_search {

namespace {

class MulriExecTest : public ValkeySearchTest {
 public:
  void SetUp() override {
    ValkeySearchTest::SetUp();
    InitThreadPools(2, 2);
    mutations_thread_pool = ValkeySearch::Instance().GetWriterThreadPool();

    std::vector<absl::string_view> key_prefixes;
    index_schema = CreateVectorHNSWSchema(index_schema_name_str, &fake_ctx_,
                                          mutations_thread_pool)
                       .value();
    VMSDK_EXPECT_OK(index_schema->Register(&fake_ctx_));
    mock_index = std::make_shared<MockIndex>();
    const char *identifier = "test_identifier";
    VMSDK_EXPECT_OK(index_schema->AddIndex("attribute_name", identifier, mock_index));
    EXPECT_CALL(*mock_index, IsTracked(testing::_))
        .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*kMockRedisModule, KeyType(testing::_))
        .WillRepeatedly(testing::Return(REDISMODULE_KEYTYPE_HASH));

    EXPECT_CALL(*kMockRedisModule,
                HashGet(testing::_, REDISMODULE_HASH_CFIELDS, testing::_,
                        testing::An<RedisModuleString **>(),
                        testing::TypedEq<void *>(nullptr)))
        .WillRepeatedly([this, identifier](RedisModuleKey *key, int flags,
                                           const char *field,
                                           RedisModuleString **value_out,
                                           void *terminating_null) {
          RedisModuleString *value_redis_str =
              TestRedisModule_CreateStringPrintf(nullptr, "%s%d", record_value_,
                                                 record_index);
          std::string field_str(field);
          std::string identifier_str(identifier);
          if (identifier_str == field_str) {
            record_index++;
          }
          *value_out = value_redis_str;
          return REDISMODULE_OK;
        });
    EXPECT_CALL(*mock_index, AddRecord(testing::_, testing::_))
        .WillRepeatedly(
            [this](const InternedStringPtr &key, absl::string_view record) {
              absl::MutexLock lock(&mutex);
              added_keys.push_back(std::string(*key));
              added_records.push_back(std::string(record));
              return true;
            });
  }
  void TearDown() override {
    ValkeySearchTest::TearDown();
    if (cb_data) {
      absl::AnyInvocable<void()> *fn = (absl::AnyInvocable<void()> *)cb_data;
      delete fn;
    }
  }
  const char *record_value_ = "value";
  vmsdk::ThreadPool *mutations_thread_pool;
  std::shared_ptr<MockIndexSchema> index_schema;
  std::shared_ptr<MockIndex> mock_index;
  void *cb_data{nullptr};
  std::vector<std::string> added_keys ABSL_GUARDED_BY(mutex);
  std::vector<std::string> added_records ABSL_GUARDED_BY(mutex);
  const std::string key_prefix = "key";
  const int max_keys = 3;
  int record_index = 0;
  const std::string index_schema_name_str{"index_schema_name"};
  mutable absl::Mutex mutex;
};

TEST_F(MulriExecTest, Basic) {
  EXPECT_CALL(*kMockRedisModule, GetContextFlags(testing::_))
      .WillRepeatedly(testing::Return(REDISMODULE_CTX_FLAGS_MULTI));
  EXPECT_CALL(*kMockRedisModule, EventLoopAddOneShot(testing::_, testing::_))
      .WillOnce([this](RedisModuleEventLoopOneShotFunc func, void *data) {
        cb_data = data;
        return REDISMODULE_OK;
      });
  std::vector<std::string> expected_keys;
  expected_keys.reserve(max_keys + 1);
  for (int i = 0; i < max_keys; ++i) {
    expected_keys.push_back(key_prefix + std::to_string(i));
  }
  EXPECT_CALL(*kMockRedisModule, BlockClient(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
      .Times(0);
  EXPECT_CALL(*kMockRedisModule,
              UnblockClient((RedisModuleBlockedClient *)1, testing::_))
      .Times(0);
  {
    absl::MutexLock lock(&mutex);
    EXPECT_TRUE(added_keys.empty());
  }
  for (size_t i = 0; i < expected_keys.size(); ++i) {
    auto key_redis_str = vmsdk::MakeUniqueRedisString(expected_keys[i]);
    index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                         "event", key_redis_str.get());
  }
  {
    absl::MutexLock lock(&mutex);
    EXPECT_TRUE(added_keys.empty());
  }
  EXPECT_EQ(mutations_thread_pool->QueueSize(), 0);
  {
    absl::MutexLock lock(&mutex);
    EXPECT_TRUE(added_keys.empty());
  }
  WaitWorkerTasksAreCompleted(*mutations_thread_pool);
  absl::AnyInvocable<void()> *fn = (absl::AnyInvocable<void()> *)cb_data;
  (*fn)();
  delete fn;
  cb_data = nullptr;
  WaitWorkerTasksAreCompleted(*mutations_thread_pool);
  {
    absl::MutexLock lock(&mutex);
    EXPECT_THAT(expected_keys, testing::UnorderedElementsAreArray(added_keys));
    added_keys.clear();

    EXPECT_CALL(*kMockRedisModule, GetContextFlags(testing::_))
        .WillOnce(testing::Return(0));
    auto key_redis_str = vmsdk::MakeUniqueRedisString("key3");
#ifdef BLOCK_CLIENT_ON_MUTATION
    EXPECT_CALL(
        *kMockRedisModule,
        BlockClient(testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return((RedisModuleBlockedClient *)1));
    EXPECT_CALL(*kMockRedisModule,
                UnblockClient((RedisModuleBlockedClient *)1, testing::_))
        .WillOnce(testing::Return(REDISMODULE_OK));
#endif
    index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                         "event", key_redis_str.get());
  }
  WaitWorkerTasksAreCompleted(*mutations_thread_pool);
  {
    absl::MutexLock lock(&mutex);
    expected_keys = {"key3"};
    EXPECT_THAT(expected_keys, testing::UnorderedElementsAreArray(added_keys));
    index_schema = nullptr;
  }
}

TEST_F(MulriExecTest, TrackMutationOverride) {
  VMSDK_EXPECT_OK(mutations_thread_pool->SuspendWorkers());
  EXPECT_CALL(*kMockRedisModule, EventLoopAddOneShot(testing::_, testing::_))
      .WillOnce([this](RedisModuleEventLoopOneShotFunc func, void *data) {
        cb_data = data;
        return REDISMODULE_OK;
      });
#ifdef BLOCK_CLIENT_ON_MUTATION
  EXPECT_CALL(*kMockRedisModule, BlockClient(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
      .Times(2)
      .WillRepeatedly(testing::Return((RedisModuleBlockedClient *)1));

  EXPECT_CALL(*kMockRedisModule,
              UnblockClient((RedisModuleBlockedClient *)1, testing::_))
      .Times(2)
      .WillRepeatedly(testing::Return(REDISMODULE_OK));
#endif
  auto key_redis_str = vmsdk::MakeUniqueRedisString(key_prefix + "0");
  index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                       "event", key_redis_str.get());
  EXPECT_EQ(mutations_thread_pool->QueueSize(), 1);

  EXPECT_CALL(*kMockRedisModule, GetContextFlags(testing::_))
      .WillRepeatedly(testing::Return(REDISMODULE_CTX_FLAGS_MULTI));

  std::vector<std::string> expected_keys;
  expected_keys.reserve(max_keys + 1);
  for (int i = 0; i < max_keys; ++i) {
    expected_keys.push_back(key_prefix + std::to_string(i));
  }
  for (const auto &key : expected_keys) {
    auto key_redis_str = vmsdk::MakeUniqueRedisString(key);
    index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                         "event", key_redis_str.get());
  }

  EXPECT_EQ(mutations_thread_pool->QueueSize(), 1);
  {
    absl::MutexLock lock(&mutex);
    EXPECT_TRUE(added_keys.empty());
  }
  EXPECT_CALL(*kMockRedisModule, GetContextFlags(testing::_))
      .WillOnce(testing::Return(0));
  key_redis_str = vmsdk::MakeUniqueRedisString(key_prefix + "1");
  VMSDK_EXPECT_OK(mutations_thread_pool->ResumeWorkers());
  index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                       "event", key_redis_str.get());
  absl::AnyInvocable<void()> *fn = (absl::AnyInvocable<void()> *)cb_data;
  (*fn)();
  delete fn;
  cb_data = nullptr;
  WaitWorkerTasksAreCompleted(*mutations_thread_pool);
  {
    absl::MutexLock lock(&mutex);
    std::vector<std::string> expected_records = {
        std::string(record_value_) + "1", std::string(record_value_) + "4",
        std::string(record_value_) + "3"};
    EXPECT_THAT(expected_keys, testing::UnorderedElementsAreArray(added_keys));
  }
  index_schema = nullptr;
}

TEST_F(MulriExecTest, FtSearchMulti) {
  EXPECT_CALL(*kMockRedisModule, EventLoopAddOneShot(testing::_, testing::_))
      .Times(0);
  VMSDK_EXPECT_OK(ValkeySearch::Instance().GetReaderThreadPool()->SuspendWorkers());
  EXPECT_CALL(
      *kMockRedisModule,
      OpenKey(&fake_ctx_, testing::An<RedisModuleString *>(), testing::_))
      .WillRepeatedly(TestRedisModule_OpenKeyDefaultImpl);
  EXPECT_CALL(*kMockRedisModule, ModuleTypeGetValue(testing::_))
      .WillRepeatedly(testing::Return(index_schema.get()));

  EXPECT_CALL(*kMockRedisModule, GetContextFlags(testing::_))
      .WillRepeatedly(testing::Return(REDISMODULE_CTX_FLAGS_MULTI));
  std::vector<std::string> expected_keys;
  expected_keys.reserve(max_keys);
  for (size_t i = 0; i < mutations_thread_pool->Size() - 1; ++i) {
    expected_keys.push_back(key_prefix + std::to_string(i));
  }
  EXPECT_CALL(*kMockRedisModule, BlockClient(testing::_, testing::_, testing::_,
                                             testing::_, testing::_))
      .Times(0);
  EXPECT_CALL(*kMockRedisModule,
              UnblockClient((RedisModuleBlockedClient *)1, testing::_))
      .Times(0);
  for (const auto &key : expected_keys) {
    auto key_redis_str = vmsdk::MakeUniqueRedisString(key);
    index_schema->OnKeyspaceNotification(&fake_ctx_, REDISMODULE_NOTIFY_HASH,
                                         "event", key_redis_str.get());
  }

  std::vector<std::string> argv = {
      "FT.SEARCH",
      "index_schema_name",
      "*=>[KNN 1 @vector $query_vector "
      "EF_RUNTIME 100 AS score]",
      "params",
      "2",
      "query_vector",
      "$embedding",
      "DIALECT",
      "2",
  };
  auto vectors = DeterministicallyGenerateVectors(1, 100, 10.0);
  std::vector<RedisModuleString *> cmd_argv;
  std::transform(
      argv.begin(), argv.end(), std::back_inserter(cmd_argv),
      [&](std::string val) {
        if (val == "$embedding") {
          return RedisModule_CreateString(&fake_ctx_, (char *)vectors[0].data(),
                                          vectors[0].size() * sizeof(float));
        }
        return RedisModule_CreateString(&fake_ctx_, val.data(), val.size());
      });
  EXPECT_FALSE(cb_data);
  VMSDK_EXPECT_OK(FTSearchCmd(&fake_ctx_, cmd_argv.data(), cmd_argv.size()));
  {
    absl::MutexLock lock(&mutex);
    EXPECT_THAT(expected_keys, testing::UnorderedElementsAreArray(added_keys));
  }
  for (auto cmd_arg : cmd_argv) {
    TestRedisModule_FreeString(&fake_ctx_, cmd_arg);
  }
  index_schema = nullptr;
}

}  // namespace
}  // namespace valkey_search
