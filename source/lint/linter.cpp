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
#include <sstream>

#include "source/diagnostic.h"
#include "source/disassemble.h"
#include "source/opcode.h"
#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/cfg.h"
#include "source/opt/control_dependence.h"
#include "source/opt/dominator_analysis.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/linter.hpp"
#include "spirv-tools/libspirv.hpp"

namespace {
bool InstructionHasDerivative(const spvtools::opt::Instruction &inst) {
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
  return std::find(std::begin(derivative_opcodes), std::end(derivative_opcodes), inst.opcode()) != std::end(derivative_opcodes);
}

bool IsValueDivergent(uint32_t id) {
  (void) id; // TODO (dongja)
  return true;
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
  std::unique_ptr<opt::IRContext> context = BuildModule(
      SPV_ENV_VULKAN_1_2, consumer(), binary, binary_size);
  if (context == nullptr) return false;

  opt::CFG &cfg = *context->cfg();
  for (opt::Function &func : *context->module()) {
    const opt::PostDominatorAnalysis &pdom = *context->GetPostDominatorAnalysis(&func);
    opt::ControlDependenceGraph cdg;
    cdg.InitializeGraph(cfg, pdom);

    std::list<opt::BasicBlock *> order;
    cfg.ComputeStructuredOrder(&func, func.entry().get(), &order);
    std::set<uint32_t> divergent_blocks;
    for (opt::BasicBlock *bb : order) {
      bool block_divergent = false;
      for (opt::ControlDependence dep : cdg.GetDependees(bb->id())) {
        if (divergent_blocks.count(dep.source) ||
            (dep.dependence_type != opt::ControlDependence::DependenceType::kEntry &&
             IsValueDivergent(dep.dependent_value_label))) {
          block_divergent = true;
          break;
        }
      }
      if (block_divergent) {
        divergent_blocks.insert(bb->id());
      }
      for (const opt::Instruction &inst : *bb) {
        if (InstructionHasDerivative(inst) && block_divergent) {
          DiagnosticStream({0, 0, 0}, impl_->consumer_, inst.PrettyPrint(), SPV_WARNING) << "derivative with non-uniform control flow";
        }
      }
    }
  }

  return true;
}

}
