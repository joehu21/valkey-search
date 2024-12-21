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

#include "src/commands/ft_search.h"

#include <strings.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "src/commands/ft_search_parser.h"
#include "src/indexes/vector_base.h"
#include "src/metrics.h"
#include "src/query/fanout.h"
#include "src/query/response_generator.h"
#include "src/query/search.h"
#include "src/schema_manager.h"
#include "src/valkey_search.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/redismodule.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/type_conversions.h"

namespace valkey_search {

namespace {
// FT.SEARCH idx "*=>[KNN 10 @vec $BLOB AS score]" PARAMS 2 BLOB
// "\x12\xa9\xf5\x6c" DIALECT 2
void ReplyAvailNeighbors(RedisModuleCtx *ctx,
                         const std::deque<indexes::Neighbor> &neighbors,
                         const query::VectorSearchParameters &parameters) {
  RedisModule_ReplyWithLongLong(
      ctx,
      std::min(neighbors.size(), static_cast<size_t>(parameters.k.value())));
}

size_t CalcEndIndex(const std::deque<indexes::Neighbor> &neighbors,
                    const query::VectorSearchParameters &parameters) {
  return std::min(static_cast<size_t>(parameters.k.value()),
                  std::min(parameters.limit.number, neighbors.size()));
}

size_t CalcStartIndex(const std::deque<indexes::Neighbor> &neighbors,
                      const query::VectorSearchParameters &parameters) {
  CHECK_GT(parameters.k.value(), parameters.limit.first_index);
  if (neighbors.size() <= parameters.limit.first_index) {
    return neighbors.size();
  }
  return parameters.limit.first_index;
}

void SendReplyNoContent(RedisModuleCtx *ctx,
                        const std::deque<indexes::Neighbor> &neighbors,
                        const query::VectorSearchParameters &parameters) {
  const size_t start_index = CalcStartIndex(neighbors, parameters);
  const size_t end_index = start_index + CalcEndIndex(neighbors, parameters);
  RedisModule_ReplyWithArray(ctx, end_index - start_index + 1);
  ReplyAvailNeighbors(ctx, neighbors, parameters);
  for (auto i = start_index; i < end_index; ++i) {
    RedisModule_ReplyWithString(
        ctx, vmsdk::MakeUniqueRedisString(*neighbors[i].external_id).get());
  }
}

void ReplyScore(RedisModuleCtx *ctx, RedisModuleString &score_as,
                const indexes::Neighbor &neighbor) {
  RedisModule_ReplyWithString(ctx, &score_as);
  auto score_value = absl::StrFormat("%.12g", neighbor.distance);
  RedisModule_ReplyWithString(ctx,
                              vmsdk::MakeUniqueRedisString(score_value).get());
}

void SerializeNeighbors(RedisModuleCtx *ctx,
                        const std::deque<indexes::Neighbor> &neighbors,
                        const query::VectorSearchParameters &parameters) {
  CHECK_GT(static_cast<size_t>(parameters.k.value()),
           parameters.limit.first_index);
  const size_t start_index = CalcStartIndex(neighbors, parameters);
  const size_t end_index = start_index + CalcEndIndex(neighbors, parameters);
  RedisModule_ReplyWithArray(ctx, 2 * (end_index - start_index) + 1);
  ReplyAvailNeighbors(ctx, neighbors, parameters);

  for (auto i = start_index; i < end_index; ++i) {
    RedisModule_ReplyWithString(
        ctx, vmsdk::MakeUniqueRedisString(*neighbors[i].external_id).get());
    if (parameters.return_attributes.empty()) {
      RedisModule_ReplyWithArray(
          ctx, 2 * neighbors[i].attribute_contents.value().size() + 2);
      ReplyScore(ctx, *parameters.score_as, neighbors[i]);
      for (auto &attribute_content : neighbors[i].attribute_contents.value()) {
        RedisModule_ReplyWithString(ctx,
                                    attribute_content.second.GetIdentifier());
        RedisModule_ReplyWithString(ctx, attribute_content.second.value.get());
      }
    } else {
      RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
      size_t cnt = 0;
      for (const auto &return_attribute : parameters.return_attributes) {
        if (vmsdk::ToStringView(parameters.score_as.get()) ==
            vmsdk::ToStringView(return_attribute.identifier.get())) {
          ReplyScore(ctx, *parameters.score_as, neighbors[i]);
          ++cnt;
          continue;
        }
        auto it = neighbors[i].attribute_contents.value().find(
            vmsdk::ToStringView(return_attribute.identifier.get()));
        if (it != neighbors[i].attribute_contents.value().end()) {
          RedisModule_ReplyWithString(ctx, return_attribute.alias.get());
          RedisModule_ReplyWithString(ctx, it->second.value.get());
          ++cnt;
        }
      }
      RedisModule_ReplySetArrayLength(ctx, 2 * cnt);
    }
  }
}

}  // namespace
// The reply structure is an array which consists of:
// 1. The amount of response elements
// 2. Per response entry:
//   1. The cache entry Hash key
//   2. An array with the following entries:
//      1. Key value: [$score_as] score_value
//      2. Distance value
//      3. Attribute name
//      4. The vector value
// SendReply respects the Limit, see https://redis.io/commands/ft.search/
void SendReply(RedisModuleCtx *ctx, std::deque<indexes::Neighbor> &neighbors,
               const query::VectorSearchParameters &parameters) {
  // Increment success counter.
  ++Metrics::GetStats().query_successful_requests_cnt;
  if (parameters.limit.first_index >=
          static_cast<uint64_t>(parameters.k.value()) ||
      parameters.limit.number == 0) {
    RedisModule_ReplyWithArray(ctx, 1);
    RedisModule_ReplyWithLongLong(ctx, neighbors.size());
    return;
  }
  if (parameters.no_content) {
    SendReplyNoContent(ctx, neighbors, parameters);
    return;
  }
  auto identifier =
      parameters.index_schema->GetIdentifier(parameters.attribute_alias);
  if (!identifier.ok()) {
    ++Metrics::GetStats().query_failed_requests_cnt;
    RedisModule_ReplyWithError(ctx, identifier.status().message().data());
    return;
  }
  query::ProcessNeighborsForReply(
      ctx, parameters.index_schema->GetAttributeDataType(), neighbors,
      parameters, identifier.value());

  SerializeNeighbors(ctx, neighbors, parameters);
}

namespace async {

int Reply(RedisModuleCtx *ctx, [[maybe_unused]] RedisModuleString **argv,
          [[maybe_unused]] int argc) {
  auto *res =
      static_cast<Result *>(RedisModule_GetBlockedClientPrivateData(ctx));
  CHECK(res != nullptr);
  if (!res->neighbors.ok()) {
    ++Metrics::GetStats().query_failed_requests_cnt;
    return RedisModule_ReplyWithError(ctx,
                                      res->neighbors.status().message().data());
  }
  SendReply(ctx, res->neighbors.value(), *res->parameters);
  return REDISMODULE_OK;
}

void Free([[maybe_unused]] RedisModuleCtx *ctx, void *privdata) {
  auto *result = static_cast<Result *>(privdata);
  delete result;
}

int Timeout(RedisModuleCtx *ctx, [[maybe_unused]] RedisModuleString **argv,
            [[maybe_unused]] int argc) {
  return RedisModule_ReplyWithSimpleString(ctx, "Request timed out");
}

}  // namespace async

absl::Status FTSearchCmd(RedisModuleCtx *ctx, RedisModuleString **argv,
                         int argc) {
  auto status = [&]() {
    auto &schema_manager = SchemaManager::Instance();
    VMSDK_ASSIGN_OR_RETURN(
        auto parameters,
        ParseVectorSearchParameters(ctx, argv + 1, argc - 1, schema_manager));

    parameters->index_schema->ProcessMultiQueue();
    bool inside_multi =
        (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_MULTI) != 0;
    if (ABSL_PREDICT_FALSE(!ValkeySearch::Instance().SupportParralelQueries() ||
                           inside_multi)) {
      VMSDK_ASSIGN_OR_RETURN(auto neighbors, query::Search(*parameters, true));
      SendReply(ctx, neighbors, *parameters);
      return absl::OkStatus();
    }

    vmsdk::BlockedClient blocked_client(ctx, async::Reply, async::Timeout,
                                        async::Free, 0);
    blocked_client.MeasureTimeStart();
    auto on_done_callback = [blocked_client = std::move(blocked_client)](
                                auto &neighbors, auto parameters) mutable {
      auto result = std::make_unique<async::Result>(async::Result{
          .neighbors = std::move(neighbors),
          .parameters = std::move(parameters),
      });
      blocked_client.SetReplyPrivateData(result.release());
    };

    if (ValkeySearch::Instance().UsingCoordinator() &&
        ValkeySearch::Instance().IsCluster() && !parameters->local_only) {
      auto search_targets = query::fanout::GetSearchTargetsForFanout(ctx);
      return query::fanout::PerformSearchFanoutAsync(
          ctx, search_targets,
          ValkeySearch::Instance().GetCoordinatorClientPool(),
          std::move(parameters), ValkeySearch::Instance().GetReaderThreadPool(),
          std::move(on_done_callback));
    }
    return query::SearchAsync(std::move(parameters),
                              ValkeySearch::Instance().GetReaderThreadPool(),
                              std::move(on_done_callback), true);
  }();
  if (!status.ok()) {
    ++Metrics::GetStats().query_failed_requests_cnt;
  }
  return status;
}

}  // namespace valkey_search
