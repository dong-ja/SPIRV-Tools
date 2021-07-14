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

#include "source/opt/control_dependence.h"

#include <cassert>
#include <utility>

#include "source/opt/basic_block.h"
#include "source/opt/cfg.h"
#include "source/opt/dominator_analysis.h"
#include "source/opt/function.h"
#include "spirv/unified1/spirv.h"

// Computes the control dependence graph (CDG). Algorithm is as presented in
// Cytron 1991, "Efficiently Computing Static Single Assignment Form and the
// Control Dependence Graph," and relies on the fact that the control dependees
// (blocks on which a block is control dependent on) are exactly the
// post-dominance frontier for that block. The explanation and proofs are given
// in Section 6 of that paper.
//
// Dominance frontier construction uses the algorithm in Section 4.2 of the same
// paper, using the post-dominance tree already constructed for us (in the IR
// context).
//
// NOTE: the implementation here follows the construction in the paper and
// includes the edge from the entry node to exit node. This differs from some
// other implementations, notably Clang Static Analyzer. This results in extra
// edges pointing from the entry node, representing a dependence on the program
// being executed.

namespace spvtools {
namespace opt {
namespace {
// Classify the given CFG edge from |source| to |target|. Returns a
// |ControlDependence|, representing an edge in the CDG, whose fields
// are filled out according to the type of edge.
static ControlDependence ClassifyControlDependence(const CFG& cfg,
                                                   uint32_t source,
                                                   uint32_t target) {
  ControlDependence dep;
  dep.source = source;
  dep.target = target;
  if (source == ControlDependenceGraph::kPseudoEntryBlock) {
    dep.dependence_type = ControlDependence::DependenceType::kEntry;
    return dep;
  }
  BasicBlock* bb = cfg.block(source);
  const Instruction& branch = *bb->rbegin();
  switch (branch.opcode()) {
    case SpvOpBranchConditional: {
      uint32_t label_true = branch.GetSingleWordInOperand(1);
      uint32_t label_false = branch.GetSingleWordInOperand(2);
      dep.dependence_type =
          ControlDependence::DependenceType::kConditionalBranch;
      dep.dependent_value_label = branch.GetSingleWordInOperand(0);
      if (target == label_true) {
        assert(target != label_false &&
               "true and false labels are the same; control dependence "
               "impossible");
        dep.condition_value = true;
      } else if (target == label_false) {
        dep.condition_value = false;
      } else {
        assert(false && "impossible control dependence; non-existent edge");
      }
      break;
    }
    case SpvOpSwitch: {
      uint32_t num_labels = (branch.NumInOperands() - 2) / 2;
      dep.dependence_type = ControlDependence::DependenceType::kSwitchCase;
      dep.dependent_value_label = branch.GetSingleWordInOperand(0);
      for (uint32_t i = 0; i < num_labels; i++) {
        uint32_t label = branch.GetSingleWordInOperand(2 + 2 * i + 1);
        uint32_t case_value = branch.GetSingleWordInOperand(2 + 2 * i);
        if (target == label) {
          dep.switch_case_values.push_back(case_value);
        }
      }
      if (target == branch.GetSingleWordInOperand(1)) {
        // default branch
        dep.is_switch_default = true;
      } else {
        assert(!dep.switch_case_values.empty() &&
               "impossible control dependence; non-existent edge");
        dep.is_switch_default = false;
      }
      break;
    }
    default:
      assert(false &&
             "invalid control dependence; opcode of last instruction is not "
             "conditional branch");
  }
  return dep;
}
}  // namespace

constexpr uint32_t ControlDependenceGraph::kPseudoEntryBlock;

bool ControlDependence::operator==(const ControlDependence& other) const {
  if (source != other.source || target != other.target ||
      dependence_type != other.dependence_type) {
    return false;
  }
  switch (dependence_type) {
    case DependenceType::kConditionalBranch:
      return dependent_value_label == other.dependent_value_label &&
             condition_value == other.condition_value;
    case DependenceType::kSwitchCase:
      return is_switch_default == other.is_switch_default &&
             switch_case_values == other.switch_case_values;
    case DependenceType::kEntry:
      return true;
    default:
      return true;
  }
}

void ControlDependenceGraph::InitializeGraph(
    const CFG& cfg, const PostDominatorAnalysis& pdom) {
  // Compute post-dominance frontiers (reverse graph).
  // The dominance frontier for a block X is equal to (Equation 4)
  //   DF_local(X) U { B in DF_up(Z) | X = ipdom(Z) }
  // where
  //   DF_local(X) = { Y | X -> Y in CFG, X does not strictly post-dominate Y }
  //   DF_up(Z) = { Y | Y in DF(Z), ipdom(Z) does not strictly post-dominate Y }
  //     (note: ipdom(Z) = X.)
  // This is computed in one pass through a post-order traversal of the
  // post-dominator tree.
  assert(!cfg.IsPseudoExitBlock(pdom.GetDomTree().post_begin()->bb_));
  Function* function = pdom.GetDomTree().post_begin()->bb_->GetParent();
  uint32_t function_entry = function->entry()->id();
  std::map<uint32_t, size_t> degree;  // Out-degree of each node
  degree[kPseudoEntryBlock] = 1;
  reverse_nodes_[kPseudoEntryBlock];  // Ensure GetDependees(0) does not crash
  for (auto it = pdom.GetDomTree().post_cbegin();
       it != pdom.GetDomTree().post_cend(); it++) {
    uint32_t label = it->id();
    degree[label] = 0;  // Ensure an entry is created for each node.
    ControlDependenceList& edges = reverse_nodes_[label];
    size_t new_size = edges.size();
    new_size += cfg.preds(label).size();
    for (DominatorTreeNode* child : *it) {
      const ControlDependenceList& child_edges = reverse_nodes_[child->id()];
      new_size += child_edges.size();
    }
    edges.reserve(new_size);
    for (uint32_t pred : cfg.preds(label)) {
      if (!pdom.StrictlyDominates(label, pred)) {
        edges.push_back(ClassifyControlDependence(cfg, pred, label));
        degree[pred]++;
      }
    }
    if (label == function_entry) {
      // Add edge from pseudo-entry to entry.
      // In CDG construction, an edge is added from entry to exit, so only the
      // exit node can post-dominate entry.
      edges.push_back(ClassifyControlDependence(cfg, kPseudoEntryBlock, label));
    }
    for (DominatorTreeNode* child : *it) {
      for (ControlDependence dep : reverse_nodes_[child->id()]) {
        dep.target = label;
        // Special-case pseudo-entry, as above.
        if (dep.source == kPseudoEntryBlock ||
            !pdom.StrictlyDominates(label, dep.source)) {
          edges.push_back(dep);
          degree[dep.source]++;
        }
      }
    }
  }
  // Compute the forward graph from the reverse graph.
  for (std::pair<uint32_t, size_t> node_degree : degree) {
    forward_nodes_[node_degree.first].reserve(node_degree.second);
  }
  for (std::pair<uint32_t, const ControlDependenceList&> entry :
       reverse_nodes_) {
    for (ControlDependence dep : entry.second) {
      forward_nodes_[dep.source].push_back(dep);
    }
  }
}

}  // namespace opt
}  // namespace spvtools
