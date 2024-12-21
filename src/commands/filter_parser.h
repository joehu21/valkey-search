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

#ifndef VALKEYSEARCH_SRC_COMMANDS_FILTER_PARSER_H_
#define VALKEYSEARCH_SRC_COMMANDS_FILTER_PARSER_H_
#include <cstddef>
#include <memory>
#include <string>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/index_schema.h"
#include "src/indexes/tag.h"
#include "src/query/predicate.h"

namespace valkey_search {
namespace indexes {
class Tag;
}  // namespace indexes
struct FilterParseResults {
  std::unique_ptr<query::Predicate> root_predicate;
  absl::flat_hash_set<std::string> filter_identifiers;
};
class FilterParser {
 public:
  FilterParser(const IndexSchema& index_schema, absl::string_view expression);

  absl::StatusOr<FilterParseResults> Parse();

 private:
  const IndexSchema& index_schema_;
  absl::string_view expression_;
  size_t pos_{0};
  absl::flat_hash_set<std::string> filter_identifiers_;

  absl::StatusOr<bool> IsMatchAllExpression();
  absl::StatusOr<std::unique_ptr<query::Predicate>> ParseExpression();
  absl::StatusOr<std::unique_ptr<query::NumericPredicate>>
  ParseNumericPredicate(const std::string& attribute_alias);
  absl::StatusOr<std::unique_ptr<query::TagPredicate>> ParseTagPredicate(
      const std::string& attribute_alias);
  void SkipWhitespace();

  char Peek() const { return expression_[pos_]; }

  bool IsEnd() const { return pos_ >= expression_.length(); }
  bool Match(char expected, bool skip_whitespace = true);
  bool MatchInsensitive(const std::string& expected);
  absl::StatusOr<std::string> ParseFieldName();

  absl::StatusOr<double> ParseNumber();

  absl::StatusOr<absl::string_view> ParseTagString();

  absl::StatusOr<absl::flat_hash_set<absl::string_view>> ParseTags(
      absl::string_view tag_string, indexes::Tag* tag_index) const;
};

}  // namespace valkey_search
#endif  // VALKEYSEARCH_SRC_COMMANDS_FILTER_PARSER_H_
