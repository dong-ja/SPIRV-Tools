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

#include "source/opt/log.h"
#include "spirv-tools/linter.hpp"
#include "tools/io.h"
#include "tools/util/cli_consumer.h"

namespace {

// Status and actions to perform after parsing command-line arguments.
enum LintActions { LINT_CONTINUE, LINT_STOP };

struct LintStatus {
  LintActions action;
  int code;
};

// Message consumer for this tool. Used to emit diagnostics.
void lint_diagnostic(spv_message_level_t level, const char* /*source*/,
                    const spv_position_t& /*position*/, const char* message) {
  if (level == SPV_MSG_ERROR) {
    fprintf(stderr, "error: ");
  }
  fprintf(stderr, "%s\n", message);
}


// Parses command-line flags. |argc| contains the number of command-line flags.
// |argv| points to an array of strings holding the flags.
//
// On return, this function stores the name of the input program in |in_file|.
// The return value indicates whether
// optimization should continue and a status code indicating an error or
// success.
LintStatus ParseFlags(int argc, const char** argv,
                      const char** in_file) {
  // TODO (dongja): actually parse flags, etc.
  if (argc != 2) {
    spvtools::Error(lint_diagnostic, nullptr, {},
                    "expected exactly one argument: in_file");
    return {LINT_STOP, 1};
  }

  *in_file = argv[1];

  return {LINT_CONTINUE, 0};
}
}

int main(int argc, const char **argv) {
  const char *in_file = nullptr;

  spvtools::Linter linter{};
  linter.SetMessageConsumer(lint_diagnostic);

  LintStatus status = ParseFlags(argc, argv, &in_file);

  if (status.action == LINT_STOP) {
    return status.code;
  }

  std::vector<uint32_t> binary;
  if (!ReadBinaryFile(in_file, &binary)) {
    return 1;
  }

  bool ok = linter.Run(binary.data(), binary.size());

  return ok ? 0 : 1;

  return 0;
}
