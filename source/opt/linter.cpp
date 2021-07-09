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

#include "source/opcode.h"
#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/dominator_analysis.h"
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

template<typename T>
void extendVector(std::vector<T> *out, const std::vector<T> &in) {
  out->reserve(out->size() + in.size());
  out->insert(out->end(), in.begin(), in.end());
}

struct CDEdge {
  uint32_t parent;
  std::string type;
};

CDEdge edgeFromNodes(const opt::CFG *cfg, uint32_t src, uint32_t dst) {
  CDEdge ret;
  ret.parent = dst;
  opt::BasicBlock *bb = cfg->block(dst);
  const opt::Instruction &branch = *bb->rbegin();
  std::ostringstream os;
  switch (branch.opcode()) {
    case SpvOpBranch:
      os << "UC";
      break;
    case SpvOpBranchConditional:
      {
        uint32_t label_true = branch.GetSingleWordInOperand(1);
        uint32_t label_false = branch.GetSingleWordInOperand(2);
        if (src == label_true && src == label_false) {
          os << "TF"; // shouldn't happen
        } else if (src == label_true) {
          os << "T";
        } else if (src == label_false) {
          os << "F";
        } else {
          os << "??";
        }
      }
      break;
    case SpvOpSwitch:
      {
        uint32_t num_labels = (branch.NumInOperands() - 2) / 2;
        for (uint32_t i = 0; i < num_labels; i++) {
          uint32_t label = branch.GetSingleWordInOperand(2 + 2 * i + 1);
          if (label == src) {
            os << "(" << branch.GetSingleWordInOperand(2 + 2 * i) << ")";
          }
        }
        if (branch.GetSingleWordOperand(1) == src) {
          os << "def";
        }
      }
      break;
    default:
      os << "?opcode " << branch.opcode();
      break;
  }
  ret.type = os.str();
  return ret;
}

bool Linter::Run(const uint32_t *binary, const size_t binary_size) const {
  using std::cout;
  std::unique_ptr<opt::IRContext> context = BuildModule(
      SPV_ENV_VULKAN_1_2, consumer(), binary, binary_size);
  if (context == nullptr) return false;

  int num = 0;

  opt::CFG *cfg = context->cfg();
  for (opt::Function &func : *context->module()) {
    cout << "// BEGIN CFG\n";
    opt::PostDominatorAnalysis *pdom = context->GetPostDominatorAnalysis(&func);
    // Compute post-dominance frontiers.
    // Algorithm from
    // "Efficiently Computing Single Static Assignment Form and the Control
    // Dependence Graph", Cytron et al 1991.
    std::map<uint32_t, std::vector<CDEdge>> df;
    for (auto it = pdom->GetDomTree().post_cbegin(); it != pdom->GetDomTree().post_cend(); it++) {
      uint32_t label = it->id();
      std::vector<CDEdge> &out = df[label];
      out.reserve(out.size() + cfg->preds(label).size());
      for (uint32_t pred : cfg->preds(label)) {
        out.push_back(edgeFromNodes(cfg, label, pred));
      }
      for (auto &child : *it) {
        const std::vector<CDEdge> &child_edges = df[child->id()];
        out.reserve(out.size() + child_edges.size());
        for (CDEdge edge : child_edges) {
          std::ostringstream os;
          os << "up " << child->id() << "[" << edge.type << "]";
          edge.type = os.str();
          out.push_back(edge);
        }
      }
      out.erase(std::remove_if(out.begin(), out.end(), [=](CDEdge &e) {
        return pdom->StrictlyDominates(label, e.parent);
      }), out.end());
    }
    cout << "digraph {\n";
    cfg->ForEachBlockInPostOrder(func.entry().get(), [=, &df](opt::BasicBlock *bb) {
      uint32_t label = bb->id();
      // draw immediate post-dominators
      uint32_t ipdom = pdom->ImmediateDominator(label)->id();
      cout << "  " << label << " -> " << ipdom << " [color=red];\n";
      cout << "  " << label << " [color = red];\n";
      // draw successors in CFG
      bb->ForEachSuccessorLabel([=](const uint32_t succ) {
        if (pdom->StrictlyDominates(succ, label)) {
          cout << label << " -> " << succ << ";\n";
        } else {
          cout << label << " -> " << succ << " [style=dashed];\n";
        }
      });
      // draw post-dominance frontiers
      for (const CDEdge &e : df[label]) {
        cout << label << " -> " << e.parent << " [color=blue, constraint=false, label=\"" << e.type << "\"];\n";
      }
    });
    cout << "}\n";
    cout << "// END CFG\n";
  }

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
