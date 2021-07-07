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

#include <algorithm>
#include <iostream>

#include "source/opt/build_module.h"
#include "source/opt/ir_context.h"
#include "spirv-tools/linter.hpp"
#include "spirv-tools/libspirv.hpp"

namespace {
bool instructionHasDerivative(const spvtools::opt::Instruction *inst) {
  static const SpvOp derivative_opcodes[] = {
    // implicit derivatives
    SpvOpImageSampleImplicitLod,
    SpvOpImageSampleDrefImplicitLod,
    SpvOpImageSampleProjImplicitLod,
    SpvOpImageSampleProjDrefImplicitLod,
    SpvOpImageSparseSampleImplicitLod,
    SpvOpImageSparseSampleDrefImplicitLod,
    SpvOpImageSparseSampleProjImplicitLod,
    SpvOpImageSparseSampleProjDrefImplicitLod,
    // explicit derivatives
    SpvOpDPdx, SpvOpDPdy, SpvOpFwidth,
    SpvOpDPdxFine, SpvOpDPdyFine, SpvOpFwidthFine,
    SpvOpDPdxCoarse, SpvOpDPdyCoarse, SpvOpFwidthCoarse,
  };
  return std::find(std::begin(derivative_opcodes), std::end(derivative_opcodes), inst->opcode()) != std::end(derivative_opcodes);
}
}

namespace spvtools {

struct Linter::Impl {
  Impl() {}
  MessageConsumer consumer_;
};

Linter::Linter() : impl_(new Impl()) {}

void Linter::SetMessageConsumer(MessageConsumer c) {
  impl_->consumer_ = std::move(c);
}

const MessageConsumer &Linter::consumer() const {
  return impl_->consumer_;
}

Linter::~Linter() {}

bool Linter::Run(const uint32_t *binary, const size_t binary_size) const {
  using std::cout;
  std::unique_ptr<opt::IRContext> context = BuildModule(
      SPV_ENV_VULKAN_1_2, consumer(), binary, binary_size);
  if (context == nullptr) return false;

  int num = 0;

  context->module()->ForEachInst([&](const opt::Instruction *inst) {
    if (!instructionHasDerivative(inst)) return;
    if (num < 100) {
      inst->Dump();
    }
    num++;
  });

  return true;
}

}
