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
#include <functional>
#include <iostream>
#include <sstream>

#include "source/diagnostic.h"
#include "source/disassemble.h"
#include "source/opcode.h"
#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/cfg.h"
#include "source/opt/control_dependence.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/dominator_analysis.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/linter.hpp"
#include "spirv-tools/libspirv.hpp"
#include "spirv/unified1/spirv.h"

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

bool InstructionIsDivergent(spvtools::opt::IRContext &context, const spvtools::opt::Instruction &inst) {
  static const SpvOp divergent_opcodes[] = {
    // function parameters
    SpvOpFunctionParameter,
  };
  if (inst.opcode() == SpvOpLoad) {
    spvtools::opt::Instruction *def = context.get_def_use_mgr()->GetDef(inst.GetSingleWordInOperand(0));
    uint32_t type_id = def->type_id();
    spvtools::opt::analysis::Pointer *type = context.get_type_mgr()->GetType(type_id)->AsPointer();
    assert(type != nullptr);
    std::vector<spvtools::opt::Instruction *> decorations = context.get_decoration_mgr()->GetDecorationsFor(inst.result_id(), false);
    bool is_flat = false;
    for (spvtools::opt::Instruction *dec : decorations) {
      if (dec->opcode() != SpvOpDecorate) continue;
      uint32_t decoration_num = dec->GetSingleWordInOperand(1);
      if (decoration_num == SpvDecorationFlat) {
        is_flat = true;
      }
    }
    switch (type->storage_class()) {
      case SpvStorageClassFunction:
      case SpvStorageClassGeneric:
      case SpvStorageClassAtomicCounter:
      case SpvStorageClassStorageBuffer:
      case SpvStorageClassPhysicalStorageBuffer:
      case SpvStorageClassOutput:
        return true;
      case SpvStorageClassInput:
        return !is_flat;
      case SpvStorageClassUniformConstant:
      case SpvStorageClassUniform:
      case SpvStorageClassWorkgroup:
      case SpvStorageClassCrossWorkgroup:
      case SpvStorageClassPrivate:
      case SpvStorageClassPushConstant:
      case SpvStorageClassImage:
      default:
        return false;
    }
    return true;
  }
  return std::find(std::begin(divergent_opcodes), std::end(divergent_opcodes), inst.opcode()) != std::end(divergent_opcodes);
}

bool InstructionIsNeverDivergent(const spvtools::opt::Instruction &inst) {
  static const SpvOp never_divergent_opcodes[] = {
    // most subgroup operations
    SpvOpSubgroupBallotKHR,
    // TODO
  };
  return std::find(std::begin(never_divergent_opcodes),
                   std::end(never_divergent_opcodes), inst.opcode()) !=
      std::end(never_divergent_opcodes);
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

// This will probably be controlled by a CLI flag eventually.
const uint32_t kPrettyPrintOptions = SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;

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
      spvtools::DiagnosticStream({0, 0, 0}, consumer, branch->PrettyPrint(kPrettyPrintOptions), SPV_WARNING) << "because %" << id << " depends on conditional branch on non-uniform value %" << blocks[id].id;
      id = blocks[id].id;
      id_type = IDType::kIDValue;
    } else {
      while (id != 0 && values[id].id_type == IDType::kIDValue) {
        if (values[id].id == 0) {
          break;
        }
        spvtools::opt::Instruction *def = def_use.GetDef(id);
        spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(kPrettyPrintOptions), SPV_WARNING) << "because %" << id << " uses %" << values[id].id << " in its definition";
        id = values[id].id;
      }
      if (id == 0) break;
      if (values[id].id == 0) {
        spvtools::opt::Instruction *def = def_use.GetDef(id);
        spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(kPrettyPrintOptions), SPV_WARNING) << "because it has a non-uniform definition";
        break;
      }
      spvtools::opt::Instruction *def = def_use.GetDef(id);
      spvtools::DiagnosticStream({0, 0, 0}, consumer, def->PrettyPrint(kPrettyPrintOptions), SPV_WARNING) << "because it is conditionally set in block %" << values[id].id << ", which is non-uniform";
      id = values[id].id;
      id_type = IDType::kIDBlock;
    }
  }
}

class DivergenceDataFlowAnalysis {
 public:
  enum class VisitResult {
    kResultChanged,
    kResultFixed,
  };

  DivergenceDataFlowAnalysis(spvtools::opt::IRContext &context, spvtools::opt::ControlDependenceGraph &cdg)
      : blocks_(), values_(), on_worklist_(), worklist_(),
      def_use_(*context.get_def_use_mgr()), cfg_(*context.cfg()),
      context_(context), cdg_(cdg) {}

  bool IsBlockDivergent(uint32_t id) const {
    return blocks_.count(id) > 0;
  }

  DivergenceGraph &divergent_block_graph() {
    return blocks_;
  }

  DivergenceGraph &divergent_value_graph() {
    return values_;
  }

 private:
  void InitializeWorklist(spvtools::opt::Function *function) {
    for (spvtools::opt::Instruction &inst : context_.types_values()) {
      Enqueue(&inst);
    }
    cfg_.ForEachBlockInReversePostOrder(function->entry().get(), [this](spvtools::opt::BasicBlock *bb) {
      for (spvtools::opt::Instruction &inst : *bb) {
        Enqueue(&inst);
      }
    });
  }

  void ForEachSuccessor(spvtools::opt::Instruction *inst, std::function<void(spvtools::opt::Instruction *)> f) {
    def_use_.ForEachUser(inst, f);
    if (inst->IsBlockTerminator()) {
      // TODO: don't count labels for branch instructions
      // we need to look at control dependencies
      inst = context_.get_instr_block(inst)->GetLabelInst();
    }
    if (inst->opcode() == SpvOpLabel) {
      uint32_t id = inst->result_id();
      for (const spvtools::opt::ControlDependence &dep : cdg_.GetDependents(id)) {
        uint32_t target = dep.target;
        spvtools::opt::Instruction *target_inst = cfg_.block(target)->GetLabelInst();
        f(target_inst);
      }
    }
  }

  VisitResult Visit(spvtools::opt::Instruction *inst) {
    if (inst->opcode() == SpvOpLabel) {
      return VisitBlock(inst->result_id());
    } else {
      return VisitInstruction(inst);
    }
  }

  VisitResult VisitBlock(uint32_t id) {
    if (blocks_.count(id)) {
      return VisitResult::kResultFixed;
    }
    for (const spvtools::opt::ControlDependence &dep : cdg_.GetDependees(id)) {
      if (blocks_.count(dep.source)) {
        blocks_[id] = { IDType::kIDBlock, dep.source, 0 };
        return VisitResult::kResultChanged;
      } else if (dep.dependence_type != spvtools::opt::ControlDependence::DependenceType::kEntry &&
                 values_.count(dep.dependent_value_label)) {
        blocks_[id] = { IDType::kIDValue, dep.dependent_value_label, dep.source };
        return VisitResult::kResultChanged;
      }
      if (dep.dependence_type != spvtools::opt::ControlDependence::DependenceType::kEntry) {
      }
    }
    return VisitResult::kResultFixed;
  }

  VisitResult VisitInstruction(spvtools::opt::Instruction *inst) {
    // if block terminator, return changed so that the block is visited
    if (inst->IsBlockTerminator()) return VisitResult::kResultChanged;
    if (!inst->HasResultId()) return VisitResult::kResultFixed;
    uint32_t id = inst->result_id();
    if (values_.count(id)) {
      return VisitResult::kResultFixed;
    }
    if (InstructionIsDivergent(context_, *inst)) {
      values_[id] = { IDType::kIDValue, 0, 0 };
      return VisitResult::kResultChanged;
    }
    if (InstructionIsNeverDivergent(*inst)) {
      return VisitResult::kResultFixed;
    }
    bool is_uniform = inst->WhileEachInId([this, id](const uint32_t *op) {
      if (!op) return true;
      if (values_.count(*op)) {
        values_[id] = { IDType::kIDValue, *op, 0 };
        return false;
      }
      if (blocks_.count(*op)) {
        values_[id] = { IDType::kIDBlock, *op, 0 };
        return false;
      }
      return true;
    });
    return is_uniform ? VisitResult::kResultFixed : VisitResult::kResultChanged;
  }

 private:

  bool Enqueue(spvtools::opt::Instruction *inst) {
    bool &is_enqueued = on_worklist_[inst];
    if (is_enqueued) return false;
    is_enqueued = true;
    worklist_.push_back(inst);
    return true;
  }

  void EnqueueSuccessors(spvtools::opt::Instruction *inst) {
    ForEachSuccessor(inst, [this](spvtools::opt::Instruction *succ) {
      Enqueue(succ);
    });
  }

 public:

  void Run(spvtools::opt::Function *function) {
    InitializeWorklist(function);
    while (!worklist_.empty()) {
      spvtools::opt::Instruction *top = worklist_.front();
      worklist_.pop_front();
      on_worklist_[top] = false;
      VisitResult result = Visit(top);
      if (result == VisitResult::kResultChanged) {
        EnqueueSuccessors(top);
      }
    }
  }

 private:
  DivergenceGraph blocks_;
  DivergenceGraph values_;
  std::map<spvtools::opt::Instruction *, bool> on_worklist_;
  std::list<spvtools::opt::Instruction *> worklist_;
  const spvtools::opt::analysis::DefUseManager &def_use_;
  spvtools::opt::CFG &cfg_;
  spvtools::opt::IRContext &context_;
  spvtools::opt::ControlDependenceGraph &cdg_;
};
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

    DivergenceDataFlowAnalysis div_analysis(*context, cdg);
    div_analysis.Run(&func);
    for (const opt::BasicBlock &bb : func) {
      for (const opt::Instruction &inst : bb) {
        if (InstructionHasDerivative(inst) && div_analysis.IsBlockDivergent(bb.id())) {
          DiagnosticStream({0, 0, 0}, impl_->consumer_, inst.PrettyPrint(), SPV_WARNING) << "derivative with non-uniform control flow"
                                                                                         << " located in block %" << bb.id();
          PrintDivergenceFlow(impl_->consumer_, cfg, *context->get_def_use_mgr(),
                              div_analysis.divergent_block_graph(),
                              div_analysis.divergent_value_graph(),
                              IDType::kIDBlock, bb.id());
        }
      }
    }
  }

  return true;
}

}
