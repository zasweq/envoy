#include "common/http/path_utility.h"

#include "test/common/http/path_utility_fuzz.pb.h"
#include "test/fuzz/fuzz_runner.h"
#include "test/fuzz/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Fuzz {
namespace {
DEFINE_PROTO_FUZZER(const test::common::http::PathUtilityTestCase& input) {
  switch (input.path_utility_selector_case()) {
  case test::common::http::PathUtilityTestCase::kCanonicalPath: {
    auto request_headers = fromHeaders<Http::TestRequestHeaderMapImpl>(
        input.canonical_path().request_headers(), {},
        {":path"}); // needs to have path header in order to be valid
    const auto result = Http::PathUtil::canonicalPath(request_headers);
    EXPECT_TRUE(result);
    EXPECT_NE(request_headers.get_(":path"), "");
    break;
  }
  case test::common::http::PathUtilityTestCase::kMergeSlashes: {
    auto request_headers = fromHeaders<Http::TestRequestHeaderMapImpl>(
        input.merge_slashes().request_headers(), {}, {":path"});
    Http::PathUtil::mergeSlashes(request_headers);
    break;
  }
  case test::common::http::PathUtilityTestCase::kRemoveQueryAndFragment: {
    auto path = input.remove_query_and_fragment().path();
    auto sanitized_path = Http::PathUtil::removeQueryAndFragment(path);
    EXPECT_NE(path.find(sanitized_path), std::string::npos);
    break;
  }
  default:
    break;
  }
}

} // namespace
} // namespace Fuzz
} // namespace Envoy