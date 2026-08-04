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

#include <sstream>
#include <string>

namespace ray {

bool ParseProcStat(std::string_view content, char *state, uint64_t *start_time_ticks) {
  // Fields after comm are numeric or single characters and never contain ')',
  // so the last ')' is always the end of comm even when comm contains ')'.
  const size_t comm_end = content.rfind(')');
  if (comm_end == std::string_view::npos) {
    return false;
  }
  std::istringstream rest(std::string(content.substr(comm_end + 1)));
  std::string field;
  rest >> field;
  if (field.empty()) {
    return false;
  }
  *state = field[0];
  // Fields 4..21 precede start time (field 22): 18 more reads.
  for (int i = 0; i < 18; i++) {
    rest >> field;
  }
  uint64_t ticks = 0;
  rest >> ticks;
  if (rest.fail()) {
    return false;
  }
  *start_time_ticks = ticks;
  return true;
}

}  // namespace ray
