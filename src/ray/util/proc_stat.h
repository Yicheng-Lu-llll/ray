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

#pragma once

#include <cstdint>
#include <string_view>

namespace ray {

/// Parse the contents of a Linux /proc/<pid>/stat file. The comm field (2)
/// may contain arbitrary characters including spaces and parentheses, so
/// parsing starts after the last ')'. On success fills the process state
/// character (field 3) and start time in clock ticks since boot (field 22)
/// and returns true; returns false when the content is malformed or
/// truncated.
bool ParseProcStat(std::string_view content, char *state, uint64_t *start_time_ticks);

}  // namespace ray
