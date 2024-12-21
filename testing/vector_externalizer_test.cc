#include "src/vector_externalizer.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "src/indexes/vector_base.h"
#include "src/utils/allocator.h"
#include "src/utils/intrusive_ref_count.h"
#include "src/utils/string_interning.h"
#include "testing/common.h"
#include "vmsdk/src/redismodule.h"
#include "vmsdk/src/testing_infra/module.h"
#include "vmsdk/src/type_conversions.h"

namespace valkey_search {

namespace {
class VectorExternalizerTest : public ValkeySearchTestWithParam<bool> {
 public:
  VectorExternalizerTest()
      : allocator(CreateUniquePtr(FixedSizeAllocator,
                                   100 * sizeof(float) + 1, true)) {}
  void SetUp() override {
    ValkeySearchTestWithParam<bool>::SetUp();
    vectors = DeterministicallyGenerateVectors(kLRUCapacity + 20, 100, 10.0);
  }
  void TearDown() override { ValkeySearchTestWithParam<bool>::TearDown(); }
  std::vector<std::vector<float>> vectors;
  RedisModuleCtx fake_ctx;
  UniqueFixedSizeAllocatorPtr allocator;
};

void ExternalizeVectors(const std::vector<std::vector<float>> &vectors,
                        size_t externalize_offset, Allocator *allocator,
                        bool normalize,
                        bool expect_externalize_success = true) {
  auto &vector_externalizer = VectorExternalizer::Instance();
  for (size_t i = externalize_offset; i < vectors.size(); ++i) {
    auto interned_key =
        StringInternStore::Intern(absl::StrCat("key-", i));
    absl::string_view vector = VectorToStr(vectors[i]);
    if (normalize) {
      float magnitude;
      auto norm_vector =
          indexes::NormalizeEmbedding(vector, sizeof(float), &magnitude);
      vector = absl::string_view((const char *)norm_vector.data(),
                                 norm_vector.size());
      auto interned_vector = StringInternStore::Intern(vector, allocator);
      EXPECT_EQ(vector_externalizer.Externalize(
                    interned_key, "attribute_identifier_1",
                    data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_HASH,
                    interned_vector, magnitude),
                expect_externalize_success);
    } else {
      auto interned_vector = StringInternStore::Intern(vector, allocator);
      EXPECT_EQ(vector_externalizer.Externalize(
                    interned_key, "attribute_identifier_1",
                    data_model::AttributeDataType::ATTRIBUTE_DATA_TYPE_HASH,
                    interned_vector, std::nullopt),
                expect_externalize_success);
    }
  }
}

void VerifyStats(const VectorExternalizer::Stats &stats) {
  auto &vector_externalizer = VectorExternalizer::Instance();
  auto stats_actual = vector_externalizer.GetStats();
  EXPECT_EQ(stats_actual.num_lru_entries, stats.num_lru_entries);
  EXPECT_EQ(stats_actual.hash_extern_errors, stats.hash_extern_errors);
  EXPECT_EQ(stats_actual.lru_promote_cnt, stats.lru_promote_cnt);
  EXPECT_EQ(stats_actual.entry_cnt, stats.entry_cnt);
  EXPECT_EQ(stats_actual.generated_value_cnt, stats.generated_value_cnt);
  EXPECT_EQ(stats_actual.deferred_entry_cnt, stats.deferred_entry_cnt);
}

TEST_P(VectorExternalizerTest, SimpleExternalize) {
  bool normalize = GetParam();
  absl::flat_hash_map<RedisModuleKey *, std::string> keys;
  EXPECT_CALL(*kMockRedisModule,
              OpenKey(VectorExternalizer::Instance().GetCtx(),
                      testing::An<RedisModuleString *>(), REDISMODULE_WRITE))
      .Times(vectors.size())
      .WillRepeatedly(
          [&](RedisModuleCtx *ctx, RedisModuleString *key, int flags) {
            auto res = TestRedisModule_OpenKeyDefaultImpl(ctx, key, flags);
            keys[res] = vmsdk::ToStringView(key);
            return res;
          });
  absl::flat_hash_map<std::string,
                      std::pair<RedisModuleHashExternCB, void *>>
      registration;
  EXPECT_CALL(*kMockRedisModule,
              HashExternalize(testing::_, testing::_, testing::_, testing::_))
      .Times(vectors.size())
      .WillRepeatedly([&](RedisModuleKey *key, RedisModuleString *field,
                          RedisModuleHashExternCB fn,
                          void *privdata) {
        registration.insert({keys[key], std::make_pair(fn, privdata)});
        auto field_str = vmsdk::ToStringView(field);
        EXPECT_EQ(field_str, "attribute_identifier_1");
        EXPECT_EQ(fn, ExternalizeCB);
        return REDISMODULE_OK;
      });
  for (size_t j = 0; j < 2; ++j) {
    ExternalizeVectors(vectors, 0, allocator.get(), normalize);
  }
  VectorExternalizer::Stats stats{.entry_cnt = vectors.size()};
  stats.deferred_entry_cnt = vectors.size();
  VerifyStats(stats);

  VectorExternalizer::Instance().ProcessEngineUpdateQueue();
  EXPECT_EQ(registration.size(), vectors.size());

  for (size_t j = 0; j < vectors.size(); ++j) {
    auto &[fn, privdata] = registration[absl::StrCat("key-", j)];
    size_t len;
    auto vector = fn(privdata, &len);
    if (normalize) {
      float magnitude_value;
      auto norm_vector = indexes::NormalizeEmbedding(
          VectorToStr(vectors[j]), sizeof(float), &magnitude_value);
      auto denorm_vector =
          DenormalizeVector(absl::string_view((const char *)norm_vector.data(),
                                              norm_vector.size()),
                            sizeof(float), magnitude_value);
      EXPECT_EQ(absl::string_view(denorm_vector.data(), denorm_vector.size()),
                absl::string_view(vector, len));
    } else {
      EXPECT_EQ(len, vectors[j].size() * sizeof(float));
      EXPECT_EQ(absl::string_view(vector, len), VectorToStr(vectors[j]));
    }
  }
  stats.deferred_entry_cnt = 0;
  stats.generated_value_cnt = vectors.size();
  if (normalize) {
    stats.num_lru_entries = kLRUCapacity;
  }
  VerifyStats(stats);
}

TEST_P(VectorExternalizerTest, PreferNotNormalized) {
  absl::flat_hash_map<RedisModuleKey *, std::string> keys;
  EXPECT_CALL(*kMockRedisModule,
              OpenKey(VectorExternalizer::Instance().GetCtx(),
                      testing::An<RedisModuleString *>(), REDISMODULE_WRITE))
      .Times(vectors.size())
      .WillRepeatedly(
          [&](RedisModuleCtx *ctx, RedisModuleString *key, int flags) {
            auto res = TestRedisModule_OpenKeyDefaultImpl(ctx, key, flags);
            keys[res] = vmsdk::ToStringView(key);
            return res;
          });
  absl::flat_hash_map<std::string,
                      std::pair<RedisModuleHashExternCB, void *>>
      registration;
  EXPECT_CALL(*kMockRedisModule,
              HashExternalize(testing::_, testing::_, testing::_, testing::_))
      .Times(vectors.size())
      .WillRepeatedly([&](RedisModuleKey *key, RedisModuleString *field,
                          RedisModuleHashExternCB fn,
                          void *privdata) {
        registration.insert({keys[key], std::make_pair(fn, privdata)});
        auto field_str = vmsdk::ToStringView(field);
        EXPECT_EQ(field_str, "attribute_identifier_1");
        EXPECT_EQ(fn, ExternalizeCB);
        return REDISMODULE_OK;
      });
  ExternalizeVectors(vectors, 0, allocator.get(), true);
  ExternalizeVectors(vectors, 0, allocator.get(), false);
  ExternalizeVectors(vectors, 0, allocator.get(), true);

  VectorExternalizer::Instance().ProcessEngineUpdateQueue();
  EXPECT_EQ(registration.size(), vectors.size());

  for (size_t j = 0; j < vectors.size(); ++j) {
    auto &[fn, privdata] = registration[absl::StrCat("key-", j)];
    size_t len;
    auto vector = fn(privdata, &len);

    EXPECT_EQ(len, vectors[j].size() * sizeof(float));
    EXPECT_EQ(absl::string_view(vector, len), VectorToStr(vectors[j]));
  }
  VectorExternalizer::Stats stats{.entry_cnt = vectors.size()};
  stats.generated_value_cnt = vectors.size();

  VerifyStats(stats);
}

void VerifyCB(
    const std::vector<std::vector<float>> &vectors, size_t offset,
    absl::flat_hash_map<std::string,
                        std::pair<RedisModuleHashExternCB, void *>>
        &registration,
    bool normalized, size_t expected_lru_promote_cnt) {
  VectorExternalizer::Instance().ProcessEngineUpdateQueue();

  auto stats = VectorExternalizer::Instance().GetStats();
  stats.entry_cnt = vectors.size();
  for (size_t j = offset; j < vectors.size(); ++j) {
    auto &[fn, privdata] = registration[absl::StrCat("key-", j)];
    size_t len;
    auto vector = fn(privdata, &len);
    if (normalized) {
      float magnitude_value;
      auto norm_vector = indexes::NormalizeEmbedding(
          VectorToStr(vectors[j]), sizeof(float), &magnitude_value);
      auto denorm_vector =
          DenormalizeVector(absl::string_view((const char *)norm_vector.data(),
                                              norm_vector.size()),
                            sizeof(float), magnitude_value);
      EXPECT_EQ(absl::string_view(denorm_vector.data(), denorm_vector.size()),
                absl::string_view(vector, len));
    } else {
      EXPECT_EQ(len, vectors[j].size() * sizeof(float));
      EXPECT_EQ(absl::string_view(vector, len), VectorToStr(vectors[j]));
    }
    ++stats.generated_value_cnt;
  }
  if (normalized) {
    stats.num_lru_entries = kLRUCapacity;
    stats.lru_promote_cnt = expected_lru_promote_cnt;
  }
  VerifyStats(stats);
}

TEST_P(VectorExternalizerTest, LRUTest) {
  bool normalized = GetParam();
  absl::flat_hash_map<RedisModuleKey *, std::string> keys;
  EXPECT_CALL(*kMockRedisModule,
              OpenKey(VectorExternalizer::Instance().GetCtx(),
                      testing::An<RedisModuleString *>(), REDISMODULE_WRITE))
      .Times(vectors.size())
      .WillRepeatedly(
          [&](RedisModuleCtx *ctx, RedisModuleString *key, int flags) {
            auto res = TestRedisModule_OpenKeyDefaultImpl(ctx, key, flags);
            keys[res] = vmsdk::ToStringView(key);
            return res;
          });
  absl::flat_hash_map<std::string,
                      std::pair<RedisModuleHashExternCB, void *>>
      registration;
  EXPECT_CALL(*kMockRedisModule,
              HashExternalize(testing::_, testing::_, testing::_, testing::_))
      .Times(vectors.size())
      .WillRepeatedly([&](RedisModuleKey *key, RedisModuleString *field,
                          RedisModuleHashExternCB fn,
                          void *privdata) {
        auto field_str = vmsdk::ToStringView(field);
        EXPECT_EQ(field_str, "attribute_identifier_1");
        EXPECT_EQ(fn, ExternalizeCB);
        registration.insert({keys[key], std::make_pair(fn, privdata)});
        return REDISMODULE_OK;
      });
  std::vector<std::vector<float>> generated_vectors;
  generated_vectors.reserve(vectors.size());
  for (size_t j = 0; j < vectors.size(); ++j) {
    generated_vectors.push_back(vectors[0]);
  }
  ExternalizeVectors(vectors, 0, allocator.get(), normalized);
  VerifyCB(vectors, 0, registration, normalized, 0);
  ExternalizeVectors(generated_vectors, generated_vectors.size() / 2,
                     allocator.get(), normalized);
  VerifyCB(generated_vectors, generated_vectors.size() / 2, registration,
           normalized, 0);
  VerifyCB(generated_vectors, generated_vectors.size() / 2, registration,
           normalized, 60);

  ExternalizeVectors(vectors, vectors.size() / 2, allocator.get(), normalized);
  VerifyCB(vectors, vectors.size() / 2, registration, normalized, 60);
  VerifyCB(vectors, vectors.size() / 2, registration, normalized, 120);
}

TEST_P(VectorExternalizerTest, OpenKeyFailure) {
  bool during_registration = GetParam();
  auto &vector_externalizer = VectorExternalizer::Instance();
  if (during_registration) {
    EXPECT_CALL(*kMockRedisModule,
                OpenKey(VectorExternalizer::Instance().GetCtx(),
                        testing::An<RedisModuleString *>(), REDISMODULE_WRITE))
        .Times(vectors.size())
        .WillRepeatedly(testing::Return(nullptr));
  } else {
    EXPECT_CALL(*kMockRedisModule,
                OpenKey(VectorExternalizer::Instance().GetCtx(),
                        testing::An<RedisModuleString *>(), REDISMODULE_WRITE))
        .WillRepeatedly(
            [&](RedisModuleCtx *ctx, RedisModuleString *key, int flags) {
              static size_t i = 0;
              return (i++ < vectors.size())
                         ? TestRedisModule_OpenKeyDefaultImpl(ctx, key, flags)
                         : nullptr;
            });
  }
  EXPECT_CALL(*kMockRedisModule,
              HashExternalize(testing::_, testing::_, testing::_, testing::_))
      .Times(during_registration ? 0 : vectors.size());

  ExternalizeVectors(vectors, 0, allocator.get(), true);
  VectorExternalizer::Instance().ProcessEngineUpdateQueue();

  auto stats = vector_externalizer.GetStats();
  if (!during_registration) {
    stats.entry_cnt = vectors.size();
  }
  VerifyStats(stats);
}

TEST_F(VectorExternalizerTest, ModuleApiNotAvailable) {
  auto &vector_externalizer = VectorExternalizer::Instance();
  EXPECT_CALL(*kMockRedisModule, GetApi(testing::_, testing::_))
      .WillOnce(testing::Return(REDISMODULE_ERR));
  EXPECT_CALL(*kMockRedisModule, GetDetachedThreadSafeContext(&fake_ctx))
      .WillOnce([&](RedisModuleCtx *ctx) { return ctx; });
  VectorExternalizer::Instance().Init(&fake_ctx);
  auto stats = vector_externalizer.GetStats();
  ExternalizeVectors(vectors, 0, allocator.get(), true, false);
  VerifyStats(stats);
}
INSTANTIATE_TEST_SUITE_P(VectorExternalizerTests, VectorExternalizerTest,
                         ::testing::Values(false, true),
                         [](const testing::TestParamInfo<bool> &info) {
                           return std::to_string(info.param);
                         });

}  // namespace
}  // namespace valkey_search
