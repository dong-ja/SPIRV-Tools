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

#include <gmock/gmock-matchers.h>

#include <algorithm>
#include <vector>

#include "gtest/gtest.h"
#include "source/opt/build_module.h"
#include "test/opt/function_utils.h"

namespace spvtools {
namespace opt {
// Readable output for test failures
static std::ostream& operator<<(std::ostream& os,
                                const ControlDependence& dep) {
  os << dep.source << "->" << dep.target;
  switch (dep.dependence_type) {
    case ControlDependence::DependenceType::kConditionalBranch:
      os << " if %" << dep.dependent_value_label << " is "
         << (dep.condition_value ? "true" : "false");
      break;
    case ControlDependence::DependenceType::kSwitchCase: {
      os << " switch %" << dep.dependent_value_label << " case ";
      bool first = true;
      for (uint32_t case_value : dep.switch_case_values) {
        if (first) {
          first = false;
        } else {
          os << ", ";
        }
        os << case_value;
      }
      if (dep.is_switch_default) {
        if (!first) {
          first = false;
          os << ", ";
        }
        os << "default";
      }
    } break;
    case ControlDependence::DependenceType::kEntry:
      os << " entry";
      break;
    default:
      os << " (unknown)";
  }
  return os;
}

bool operator<(const ControlDependence& dep1, const ControlDependence& dep2) {
  // Compare dep1 < dep2 lexicographically.
  if (dep1.source != dep2.source) return dep1.source < dep2.source;
  return dep1.target < dep2.target;
}

namespace {
static void GatherEdges(const ControlDependenceGraph& cdg,
                        std::vector<ControlDependence>* ret) {
  cdg.ForEachBlockLabel([&](uint32_t label) {
    ret->reserve(ret->size() + cdg.GetDependents(label).size());
    ret->insert(ret->end(), cdg.GetDependents(label).begin(),
                cdg.GetDependents(label).end());
  });
  std::sort(ret->begin(), ret->end());
  // Verify that reverse graph is the same.
  std::vector<ControlDependence> reverse_edges;
  reverse_edges.reserve(ret->size());
  cdg.ForEachBlockLabel([&](uint32_t label) {
    reverse_edges.insert(reverse_edges.end(), cdg.GetDependees(label).begin(),
                         cdg.GetDependees(label).end());
  });
  std::sort(reverse_edges.begin(), reverse_edges.end());
  ASSERT_THAT(reverse_edges, testing::ElementsAreArray(*ret));
}

static ControlDependence MakeCondBranchDep(uint32_t source, uint32_t target,
                                           uint32_t condition_label,
                                           bool value) {
  ControlDependence dep;
  dep.source = source;
  dep.target = target;
  dep.dependence_type = ControlDependence::DependenceType::kConditionalBranch;
  dep.dependent_value_label = condition_label;
  dep.condition_value = value;
  return dep;
}

static ControlDependence MakeSwitchCaseDep(uint32_t source, uint32_t target,
                                           uint32_t switch_value,
                                           bool is_default,
                                           std::vector<uint32_t> cases) {
  ControlDependence dep;
  dep.source = source;
  dep.target = target;
  dep.dependence_type = ControlDependence::DependenceType::kSwitchCase;
  dep.dependent_value_label = switch_value;
  dep.is_switch_default = is_default;
  dep.switch_case_values = cases;
  return dep;
}

static ControlDependence MakeEntryDep(uint32_t target) {
  ControlDependence dep;
  dep.source = ControlDependenceGraph::kPseudoEntryBlock;
  dep.target = target;
  dep.dependence_type = ControlDependence::DependenceType::kEntry;
  return dep;
}

using ControlDependenceTest = ::testing::Test;

TEST(ControlDependenceTest, DependenceSimpleCFG) {
  const std::string text = R"(
               OpCapability Addresses
               OpCapability Kernel
               OpMemoryModel Physical64 OpenCL
               OpEntryPoint Kernel %1 "main"
          %2 = OpTypeVoid
          %3 = OpTypeFunction %2
          %4 = OpTypeBool
          %5 = OpTypeInt 32 0
          %6 = OpConstant %5 0
          %7 = OpConstantFalse %4
          %8 = OpConstantTrue %4
          %9 = OpConstant %5 1
          %1 = OpFunction %2 None %3
         %10 = OpLabel
               OpBranch %11
         %11 = OpLabel
               OpSwitch %6 %12 1 %13
         %12 = OpLabel
               OpBranch %14
         %13 = OpLabel
               OpBranch %14
         %14 = OpLabel
               OpBranchConditional %8 %15 %16
         %15 = OpLabel
               OpBranch %19
         %16 = OpLabel
               OpBranchConditional %8 %17 %18
         %17 = OpLabel
               OpBranch %18
         %18 = OpLabel
               OpBranch %19
         %19 = OpLabel
               OpReturn
               OpFunctionEnd
)";
  std::unique_ptr<IRContext> context =
      BuildModule(SPV_ENV_UNIVERSAL_1_0, nullptr, text,
                  SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS);
  Module* module = context->module();
  EXPECT_NE(nullptr, module) << "Assembling failed for shader:\n"
                             << text << std::endl;
  const Function* fn = spvtest::GetFunction(module, 1);
  const BasicBlock* entry = spvtest::GetBasicBlock(fn, 10);
  EXPECT_EQ(entry, fn->entry().get())
      << "The entry node is not the expected one";

  {
    PostDominatorAnalysis pdom;
    const CFG& cfg = *context->cfg();
    pdom.InitializeTree(cfg, fn);
    ControlDependenceGraph cdg;
    cdg.InitializeGraph(cfg, pdom);

    EXPECT_TRUE(cdg.IsDependent(12, 11));
    EXPECT_TRUE(cdg.IsDependent(13, 11));
    EXPECT_TRUE(cdg.IsDependent(15, 14));
    EXPECT_TRUE(cdg.IsDependent(16, 14));
    EXPECT_TRUE(cdg.IsDependent(18, 14));
    EXPECT_TRUE(cdg.IsDependent(17, 16));
    EXPECT_TRUE(cdg.IsDependent(10, 0));
    EXPECT_TRUE(cdg.IsDependent(11, 0));
    EXPECT_TRUE(cdg.IsDependent(14, 0));
    EXPECT_TRUE(cdg.IsDependent(19, 0));
    EXPECT_FALSE(cdg.IsDependent(14, 11));
    EXPECT_FALSE(cdg.IsDependent(17, 14));
    EXPECT_FALSE(cdg.IsDependent(19, 14));
    EXPECT_FALSE(cdg.IsDependent(12, 0));

    std::vector<ControlDependence> edges;
    GatherEdges(cdg, &edges);
    EXPECT_THAT(edges,
                testing::ElementsAre(MakeEntryDep(10), MakeEntryDep(11),
                                     MakeEntryDep(14), MakeEntryDep(19),
                                     MakeSwitchCaseDep(11, 12, 6, true, {}),
                                     MakeSwitchCaseDep(11, 13, 6, false, {1}),
                                     MakeCondBranchDep(14, 15, 8, true),
                                     MakeCondBranchDep(14, 16, 8, false),
                                     MakeCondBranchDep(14, 18, 8, false),
                                     MakeCondBranchDep(16, 17, 8, true)));
  }
}

TEST(ControlDependenceTest, DependencePaperCFG) {
  const std::string text = R"(
               OpCapability Addresses
               OpCapability Kernel
               OpMemoryModel Physical64 OpenCL
               OpEntryPoint Kernel %101 "main"
        %102 = OpTypeVoid
        %103 = OpTypeFunction %102
        %104 = OpTypeBool
        %108 = OpConstantTrue %104
        %101 = OpFunction %102 None %103
          %1 = OpLabel
               OpBranch %2
          %2 = OpLabel
               OpBranchConditional %108 %3 %7
          %3 = OpLabel
               OpBranchConditional %108 %4 %5
          %4 = OpLabel
               OpBranch %6
          %5 = OpLabel
               OpBranch %6
          %6 = OpLabel
               OpBranch %8
          %7 = OpLabel
               OpBranch %8
          %8 = OpLabel
               OpBranch %9
          %9 = OpLabel
               OpBranchConditional %108 %10 %11
         %10 = OpLabel
               OpBranch %11
         %11 = OpLabel
               OpBranchConditional %108 %12 %9
         %12 = OpLabel
               OpBranchConditional %108 %13 %2
         %13 = OpLabel
               OpReturn
               OpFunctionEnd
)";
  std::unique_ptr<IRContext> context =
      BuildModule(SPV_ENV_UNIVERSAL_1_0, nullptr, text,
                  SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS);
  Module* module = context->module();
  EXPECT_NE(nullptr, module) << "Assembling failed for shader:\n"
                             << text << std::endl;
  const Function* fn = spvtest::GetFunction(module, 101);
  const BasicBlock* entry = spvtest::GetBasicBlock(fn, 1);
  EXPECT_EQ(entry, fn->entry().get())
      << "The entry node is not the expected one";

  {
    PostDominatorAnalysis pdom;
    const CFG& cfg = *context->cfg();
    pdom.InitializeTree(cfg, fn);
    ControlDependenceGraph cdg;
    cdg.InitializeGraph(cfg, pdom);

    std::vector<ControlDependence> edges;
    GatherEdges(cdg, &edges);
    EXPECT_THAT(edges, testing::ElementsAre(
                           MakeEntryDep(1), MakeEntryDep(2), MakeEntryDep(8),
                           MakeEntryDep(9), MakeEntryDep(11), MakeEntryDep(12),
                           MakeEntryDep(13), MakeCondBranchDep(2, 3, 108, true),
                           MakeCondBranchDep(2, 6, 108, true),
                           MakeCondBranchDep(2, 7, 108, false),
                           MakeCondBranchDep(3, 4, 108, true),
                           MakeCondBranchDep(3, 5, 108, false),
                           MakeCondBranchDep(9, 10, 108, true),
                           MakeCondBranchDep(11, 9, 108, false),
                           MakeCondBranchDep(11, 11, 108, false),
                           MakeCondBranchDep(12, 2, 108, false),
                           MakeCondBranchDep(12, 8, 108, false),
                           MakeCondBranchDep(12, 9, 108, false),
                           MakeCondBranchDep(12, 11, 108, false),
                           MakeCondBranchDep(12, 12, 108, false)));
  }
}

}  // namespace
}  // namespace opt
}  // namespace spvtools
