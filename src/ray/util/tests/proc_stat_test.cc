// Copyright 2026 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/util/proc_stat.h"

#include <string>

#include "gtest/gtest.h"

namespace ray {

namespace {

// A realistic stat line: pid 1234, comm "ray::IDLE", state S, fields 4..21,
// then starttime (field 22) = 4567, followed by trailing fields.
std::string StatLine(const std::string &comm, char state, uint64_t starttime) {
  std::string line = "1234 (" + comm + ") " + state;
  for (int field = 4; field <= 21; field++) {
    line += " " + std::to_string(field);
  }
  line += " " + std::to_string(starttime) + " 99 100";
  return line;
}

}  // namespace

TEST(ParseProcStatTest, ParsesStateAndStartTime) {
  char state = 0;
  uint64_t ticks = 0;
  ASSERT_TRUE(ParseProcStat(StatLine("raylet", 'S', 4567), &state, &ticks));
  EXPECT_EQ(state, 'S');
  EXPECT_EQ(ticks, 4567u);
}

TEST(ParseProcStatTest, CommWithSpacesAndParens) {
  char state = 0;
  uint64_t ticks = 0;
  ASSERT_TRUE(ParseProcStat(StatLine("ray::IDLE) x (y", 'Z', 42), &state, &ticks));
  EXPECT_EQ(state, 'Z');
  EXPECT_EQ(ticks, 42u);
}

TEST(ParseProcStatTest, TruncatedLineFails) {
  char state = 0;
  uint64_t ticks = 0;
  // Cut before field 22.
  const std::string full = StatLine("raylet", 'R', 4567);
  EXPECT_FALSE(ParseProcStat(full.substr(0, full.size() - 12), &state, &ticks));
}

TEST(ParseProcStatTest, NoCommFails) {
  char state = 0;
  uint64_t ticks = 0;
  EXPECT_FALSE(ParseProcStat("1234 no-parens R 1 2 3", &state, &ticks));
}

TEST(ParseProcStatTest, EmptyAfterCommFails) {
  char state = 0;
  uint64_t ticks = 0;
  EXPECT_FALSE(ParseProcStat("1234 (raylet)", &state, &ticks));
}

}  // namespace ray
