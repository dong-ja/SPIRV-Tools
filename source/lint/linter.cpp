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

enum class IDType {
  kIDBlock,
  kIDValue,
};

// Represents why a block or value is divergent.
struct DivergenceEdge {
  IDType id_type;
  uint32_t id;
  uint32_t source_id; // for block -> value edges, the ID of the block containing the conditional branch
};

using DivergenceGraph = std::map<uint32_t, DivergenceEdge>;

bool IsValueDivergent(uint32_t id, DivergenceGraph &divergent_values) {
  if (divergent_values.count(id)) {
    return true;
  }
  divergent_values[id] = { IDType::kIDValue, 0, 0 }; // root
  return true;
}

void PrintDivergenceFlow(spvtools::MessageConsumer &consumer, spvtools::opt::CFG &cfg, spvtools::opt::analysis::DefUseManager &def_use, DivergenceGraph blocks, DivergenceGraph values, IDType id_type, uint32_t id) {
  while (id != 0) {
    spvtools::DiagnosticStream({0, 0, 0}, consumer, "", SPV_WARNING) << (id_type == IDType::kIDBlock ? "block" : "value") << " %" << id << " is non-uniform";
    if (id_type == IDType::kIDBlock) {
      while (id != 0 && blocks[id].id_type == IDType::kIDBlock) {
        if (blocks[id].id == 0) {
          break;
        }
        id = blocks[id].id;
      }
      if (id == 0 || blocks[id].id == 0) break;
      spvtools::opt::Instruction *branch = cfg.block(blocks[id].source_id)->terminator();
      spvtools::DiagnosticStream({0, 0, 0}, consumer, branch->PrettyPrint(), SPV_WARNING) << "because %" << id << " depends on conditional branch on non-uniform value %" << blocks[id].id;
      id = blocks[id].id;
      id_type = IDType::kIDValue;
    } else {
      while (id != 0 && values[id].id_type == IDType::kIDValue) {
        if (values[id].id == 0) {
          break;
        }
        spvtools::opt::Instruction *def = def_use.GetDef(id);
        spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(), SPV_WARNING) << "because %" << id << " uses %" << values[id].id << " in its definition";
        id = values[id].id;
      }
      if (id == 0) break;
      if (values[id].id == 0) {
        spvtools::opt::Instruction *def = def_use.GetDef(id);
        spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(), SPV_WARNING) << "because it has a non-uniform definition";
        break;
      }
      spvtools::opt::Instruction *def = def_use.GetDef(id);
      spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(), SPV_WARNING) << "because it is conditionally set in block %" << values[id].id << ", which is non-uniform";
      id = values[id].id;
      id_type = IDType::kIDBlock;
    }
  }
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
    DivergenceGraph divergent_blocks;
    DivergenceGraph divergent_values;
    for (opt::BasicBlock *bb : order) {
      if (!cdg.DoesBlockExist(bb->id())) {
        continue;
      }
      bool block_divergent = false;
      for (opt::ControlDependence dep : cdg.GetDependees(bb->id())) {
        if (divergent_blocks.count(dep.source)) {
          block_divergent = true;
          divergent_blocks[bb->id()] = { IDType::kIDBlock, dep.source, 0 };
          break;
        } else if (dep.dependence_type != opt::ControlDependence::DependenceType::kEntry &&
                   IsValueDivergent(dep.dependent_value_label, divergent_values)) {
          block_divergent = true;
          divergent_blocks[bb->id()] = { IDType::kIDValue, dep.dependent_value_label, dep.source };
          break;
        }
      }
      for (const opt::Instruction &inst : *bb) {
        if (InstructionHasDerivative(inst) && block_divergent) {
          DiagnosticStream({0, 0, 0}, impl_->consumer_, inst.PrettyPrint(), SPV_WARNING) << "derivative with non-uniform control flow"
                                                                                         << " located in block %" << bb->id();
          PrintDivergenceFlow(impl_->consumer_, cfg, *context->get_def_use_mgr(), divergent_blocks, divergent_values, IDType::kIDBlock, bb->id());
        }
      }
    }
  }

  return true;
}

}
