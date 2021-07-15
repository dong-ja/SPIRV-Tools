// Copyright (c) 2021 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDE_SPIRV_TOOLS_LINTER_HPP_
#define INCLUDE_SPIRV_TOOLS_LINTER_HPP_

#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "libspirv.hpp"

namespace spvtools {

// C++ interface for SPIR-V linting functionalities.
class Linter {
 public:
  // Constructs an instance with the given target |env|, which is used to decode
  // the binaries to be optimized later.
  //
  // The instance will have an empty message consumer, which ignores all
  // messages from the library. Use SetMessageConsumer() to supply a consumer
  // if messages are of concern.
  explicit Linter();

  // Disables copy/move constructor/assignment operations.
  Linter(const Linter&) = delete;
  Linter(Linter&&) = delete;
  Linter& operator=(const Linter&) = delete;
  Linter& operator=(Linter&&) = delete;

  // Destructs this instance.
  ~Linter();

  // Sets the message consumer to the given |consumer|. The |consumer| will be
  // invoked once for each message communicated from the library.
  void SetMessageConsumer(MessageConsumer consumer);

  // Returns a reference to the registered message consumer.
  const MessageConsumer& consumer() const;

  // Lints the given SPIR-V module |original_binary|.
  //
  // Returns true if all lints succeed (i.e. output no errors).
  // Returns false if any lint fails.
  bool Run(const uint32_t* binary, size_t binary_size) const;

 private:
  struct Impl;                  // Opaque struct for holding internal data.
  std::unique_ptr<Impl> impl_;  // Unique pointer to internal data.
};

}  // namespace spvtools

#endif  // INCLUDE_SPIRV_TOOLS_OPTIMIZER_HPP_
