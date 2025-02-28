// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/hset_family.h"

extern "C" {
#include "redis/listpack.h"
#include "redis/object.h"
#include "redis/sds.h"
}

#include "base/gtest.h"
#include "base/logging.h"
#include "facade/facade_test.h"
#include "server/test_utils.h"

using namespace testing;
using namespace std;
using namespace util;
using namespace boost;
using namespace facade;

namespace dfly {

class HSetFamilyTest : public BaseFamilyTest {
 protected:
};

class HestFamilyTestProtocolVersioned : public HSetFamilyTest,
                                        public ::testing::WithParamInterface<string> {
 protected:
};

INSTANTIATE_TEST_CASE_P(HestFamilyTestProtocolVersioned, HestFamilyTestProtocolVersioned,
                        ::testing::Values("2", "3"));

TEST_F(HSetFamilyTest, Hash) {
  robj* obj = createHashObject();
  sds field = sdsnew("field");
  sds val = sdsnew("value");
  hashTypeSet(obj, field, val, 0);
  sdsfree(field);
  sdsfree(val);
  decrRefCount(obj);
}

TEST_F(HSetFamilyTest, Basic) {
  auto resp = Run({"hset", "x", "a"});
  EXPECT_THAT(resp, ErrArg("wrong number"));

  EXPECT_THAT(Run({"HSET", "hs", "key1", "val1", "key2"}), ErrArg("wrong number"));

  EXPECT_EQ(1, CheckedInt({"hset", "x", "a", "b"}));
  EXPECT_EQ(1, CheckedInt({"hlen", "x"}));

  EXPECT_EQ(1, CheckedInt({"hexists", "x", "a"}));
  EXPECT_EQ(0, CheckedInt({"hexists", "x", "b"}));
  EXPECT_EQ(0, CheckedInt({"hexists", "y", "a"}));

  EXPECT_EQ(0, CheckedInt({"hset", "x", "a", "b"}));
  EXPECT_EQ(0, CheckedInt({"hset", "x", "a", "c"}));
  EXPECT_EQ(0, CheckedInt({"hset", "x", "a", ""}));

  EXPECT_EQ(2, CheckedInt({"hset", "y", "a", "c", "d", "e"}));
  EXPECT_EQ(2, CheckedInt({"hdel", "y", "a", "d"}));

  EXPECT_THAT(Run({"hdel", "nokey", "a"}), IntArg(0));
}

TEST_F(HSetFamilyTest, HSet) {
  string val(1024, 'b');

  EXPECT_EQ(1, CheckedInt({"hset", "large", "a", val}));
  EXPECT_EQ(1, CheckedInt({"hlen", "large"}));
  EXPECT_EQ(1024, CheckedInt({"hstrlen", "large", "a"}));

  EXPECT_EQ(1, CheckedInt({"hset", "small", "", "565323349817"}));
}

TEST_P(HestFamilyTestProtocolVersioned, Get) {
  auto resp = Run({"hello", GetParam()});
  EXPECT_THAT(resp.GetVec()[6], "proto");
  EXPECT_THAT(resp.GetVec()[7], IntArg(atoi(GetParam().c_str())));

  resp = Run({"hset", "x", "a", "1", "b", "2", "c", "3"});
  EXPECT_THAT(resp, IntArg(3));

  resp = Run({"hmget", "unkwn", "a", "c"});
  ASSERT_THAT(resp, ArgType(RespExpr::ARRAY));
  EXPECT_THAT(resp.GetVec(), ElementsAre(ArgType(RespExpr::NIL), ArgType(RespExpr::NIL)));

  resp = Run({"hkeys", "x"});
  ASSERT_THAT(resp, ArgType(RespExpr::ARRAY));
  EXPECT_THAT(resp.GetVec(), UnorderedElementsAre("a", "b", "c"));

  resp = Run({"hvals", "x"});
  ASSERT_THAT(resp, ArgType(RespExpr::ARRAY));
  EXPECT_THAT(resp.GetVec(), UnorderedElementsAre("1", "2", "3"));

  resp = Run({"hmget", "x", "a", "c", "d"});
  ASSERT_THAT(resp, ArgType(RespExpr::ARRAY));
  EXPECT_THAT(resp.GetVec(), ElementsAre("1", "3", ArgType(RespExpr::NIL)));

  resp = Run({"hgetall", "x"});
  ASSERT_THAT(resp, ArgType(RespExpr::ARRAY));
  EXPECT_THAT(resp.GetVec(), ElementsAre("a", "1", "b", "2", "c", "3"));
}

TEST_F(HSetFamilyTest, HSetNx) {
  EXPECT_EQ(1, CheckedInt({"hsetnx", "key", "field", "val"}));
  EXPECT_EQ(Run({"hget", "key", "field"}), "val");

  EXPECT_EQ(0, CheckedInt({"hsetnx", "key", "field", "val2"}));
  EXPECT_EQ(Run({"hget", "key", "field"}), "val");

  EXPECT_EQ(1, CheckedInt({"hsetnx", "key", "field2", "val2"}));
  EXPECT_EQ(Run({"hget", "key", "field2"}), "val2");

  // check dict path
  EXPECT_EQ(0, CheckedInt({"hsetnx", "key", "field2", string(512, 'a')}));
  EXPECT_EQ(Run({"hget", "key", "field2"}), "val2");
}

TEST_F(HSetFamilyTest, HIncr) {
  EXPECT_EQ(10, CheckedInt({"hincrby", "key", "field", "10"}));

  Run({"hset", "key", "a", " 1"});
  auto resp = Run({"hincrby", "key", "a", "10"});
  EXPECT_THAT(resp, ErrArg("hash value is not an integer"));
}

TEST_F(HSetFamilyTest, HScan) {
  for (int i = 0; i < 10; i++) {
    Run({"HSET", "myhash", absl::StrCat("Field-", i), absl::StrCat("Value-", i)});
  }

  // Note that even though this limit by 4, it would return more because
  // all fields are on listpack
  auto resp = Run({"hscan", "myhash", "0", "count", "4"});
  EXPECT_THAT(resp, ArrLen(2));
  auto vec = StrArray(resp.GetVec()[1]);
  EXPECT_EQ(vec.size(), 20);
  EXPECT_THAT(vec, Each(AnyOf(StartsWith("Field"), StartsWith("Value"))));

  // Now run with filter on the results - we are expecting to not getting
  // any result at this point
  resp = Run({"hscan", "myhash", "0", "match", "*x*"});  // nothing should match this
  EXPECT_THAT(resp, ArrLen(2));
  vec = StrArray(resp.GetVec()[1]);
  EXPECT_EQ(vec.size(), 0);

  // now we will do a positive match - anything that has 1 on it
  resp = Run({"hscan", "myhash", "0", "match", "*1*"});
  EXPECT_THAT(resp, ArrLen(2));
  vec = StrArray(resp.GetVec()[1]);
  EXPECT_EQ(vec.size(), 2);  // key/value = 2

  // Test with large hash to see that count limit the number of entries
  for (int i = 0; i < 200; i++) {
    Run({"HSET", "largehash", absl::StrCat("KeyNum-", i), absl::StrCat("KeyValue-", i)});
  }
  resp = Run({"hscan", "largehash", "0", "count", "20"});
  EXPECT_THAT(resp, ArrLen(2));
  vec = StrArray(resp.GetVec()[1]);

  // See https://redis.io/commands/scan/ --> "The COUNT option", for why this cannot be exact
  EXPECT_GE(vec.size(), 40);  // This should be larger than (20 * 2) and less than about 50
  EXPECT_LT(vec.size(), 60);
}

TEST_F(HSetFamilyTest, HScanLpMatchBug) {
  Run({"HSET", "key", "1", "2"});
  auto resp = Run({"hscan", "key", "0", "match", "1"});
  EXPECT_THAT(resp, ArrLen(2));
}

TEST_F(HSetFamilyTest, HincrbyFloat) {
  Run({"hincrbyfloat", "k", "a", "1.5"});
  EXPECT_EQ(Run({"hget", "k", "a"}), "1.5");

  Run({"hincrbyfloat", "k", "a", "1.5"});
  EXPECT_EQ(Run({"hget", "k", "a"}), "3");

  for (size_t i = 0; i < 500; ++i) {
    Run({"hincrbyfloat", "k", absl::StrCat("v", i), "1.5"});
  }

  for (size_t i = 0; i < 500; ++i) {
    EXPECT_EQ(Run({"hget", "k", absl::StrCat("v", i)}), "1.5");
  }
}

TEST_F(HSetFamilyTest, HRandFloat) {
  Run({"HSET", "k", "1", "2"});

  EXPECT_EQ(Run({"hrandfield", "k"}), "1");

  for (size_t i = 0; i < 500; ++i) {
    Run({"hincrbyfloat", "k", absl::StrCat("v", i), "1.1"});
  }

  Run({"hrandfield", "k"});
}

TEST_F(HSetFamilyTest, HSetEx) {
  TEST_current_time_ms = kMemberExpiryBase * 1000;  // to reset to test time.

  EXPECT_THAT(Run({"HSETEX", "k", "1", "f", "v"}), IntArg(1));

  AdvanceTime(500);
  EXPECT_THAT(Run({"HGET", "k", "f"}), "v");

  AdvanceTime(500);
  EXPECT_THAT(Run({"HGET", "k", "f"}), ArgType(RespExpr::NIL));
}

}  // namespace dfly
