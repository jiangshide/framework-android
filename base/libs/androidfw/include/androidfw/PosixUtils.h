/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <memory>
#include <string>
#include <vector>

namespace android {
namespace util {

struct ProcResult {
  int status;
  std::string stdout;
  std::string stderr;
};

// Fork, exec and wait for an external process. Return nullptr if the process could not be launched,
// otherwise a ProcResult containing the external process' exit status and captured stdout and
// stderr.
std::unique_ptr<ProcResult> ExecuteBinary(const std::vector<std::string>& argv);

} // namespace util
} // namespace android
