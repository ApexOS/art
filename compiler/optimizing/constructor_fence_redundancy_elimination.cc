/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "constructor_fence_redundancy_elimination.h"

#include "base/arena_allocator.h"
#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"

namespace art HIDDEN {

static constexpr bool kCfreLogFenceInputCount = false;

// TODO: refactor this code by reusing escape analysis.
class CFREVisitor final : public HGraphVisitor {
 public:
  CFREVisitor(HGraph* graph, OptimizingCompilerStats* stats)
      : HGraphVisitor(graph),
        scoped_allocator_(graph->GetArenaStack()),
        candidate_fences_(scoped_allocator_.Adapter(kArenaAllocCFRE)),
        candidate_fence_targets_(std::nullopt),
        stats_(stats) {}

  void VisitBasicBlock(HBasicBlock* block) override {
    // Visit all non-Phi instructions in the block.
    VisitNonPhiInstructions(block);

    // If there were any unmerged fences left, merge them together,
    // the objects are considered 'published' at the end of the block.
    MergeCandidateFences();
  }

  void VisitConstructorFence(HConstructorFence* constructor_fence) override {
    candidate_fences_.push_back(constructor_fence);

    if (!candidate_fence_targets_.has_value()) {
      // Small pre-allocated initial buffer avoids initializing a large buffer
      // until it's really needed.
      static constexpr size_t kInitialBufferSize = 16;
      HInstruction** initial_buffer =
         scoped_allocator_.AllocArray<HInstruction*>(kInitialBufferSize, kArenaAllocCFRE);
      candidate_fence_targets_.emplace(
          initial_buffer, kInitialBufferSize, scoped_allocator_.Adapter(kArenaAllocCFRE));
    }

    for (size_t input_idx = 0; input_idx < constructor_fence->InputCount(); ++input_idx) {
      candidate_fence_targets_->insert(constructor_fence->InputAt(input_idx));
    }
  }

  void VisitBoundType(HBoundType* bound_type) override {
    VisitAlias(bound_type);
  }

  void VisitNullCheck(HNullCheck* null_check) override {
    VisitAlias(null_check);
  }

  void VisitSelect(HSelect* select) override {
    VisitAlias(select);
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* instruction) override {
    HInstruction* value = instruction->InputAt(1);
    VisitSetLocation(instruction, value);
  }

  void VisitStaticFieldSet(HStaticFieldSet* instruction) override {
    HInstruction* value = instruction->InputAt(1);
    VisitSetLocation(instruction, value);
  }

  void VisitArraySet(HArraySet* instruction) override {
    HInstruction* value = instruction->InputAt(2);
    VisitSetLocation(instruction, value);
  }

  void VisitDeoptimize(HDeoptimize* instruction ATTRIBUTE_UNUSED) override {
    // Pessimize: Merge all fences.
    MergeCandidateFences();
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitInvokeVirtual(HInvokeVirtual* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitInvokeInterface(HInvokeInterface* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitInvokeUnresolved(HInvokeUnresolved* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitInvokePolymorphic(HInvokePolymorphic* invoke) override {
    HandleInvoke(invoke);
  }

  void VisitClinitCheck(HClinitCheck* clinit) override {
    HandleInvoke(clinit);
  }

  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedInstanceFieldSet(HUnresolvedInstanceFieldSet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

  void VisitUnresolvedStaticFieldSet(HUnresolvedStaticFieldSet* instruction) override {
    // Conservatively treat it as an invocation.
    HandleInvoke(instruction);
  }

 private:
  void HandleInvoke(HInstruction* invoke) {
    // An object is considered "published" if it escapes into an invoke as any of the parameters.
    if (HasInterestingPublishTargetAsInput(invoke)) {
        MergeCandidateFences();
    }
  }

  // Called by any instruction visitor that may create an alias.
  // These instructions may create an alias:
  // - BoundType
  // - NullCheck
  // - Select
  //
  // These also create an alias, but are not handled by this function:
  // - Phi: propagates values across blocks, but we always merge at the end of a block.
  // - Invoke: this is handled by HandleInvoke.
  void VisitAlias(HInstruction* aliasing_inst) {
    // An object is considered "published" if it becomes aliased by other instructions.
    if (HasInterestingPublishTargetAsInput(aliasing_inst))  {
      MergeCandidateFences();
    }
  }

  void VisitSetLocation([[maybe_unused]] HInstruction* inst, HInstruction* store_input) {
    if (candidate_fences_.empty()) {
      // There is no need to look at inputs if there are no candidate fence targets.
      DCHECK_IMPLIES(candidate_fence_targets_.has_value(), candidate_fence_targets_->empty());
      return;
    }
    // An object is considered "published" if it's stored onto the heap.
    // Sidenote: A later "LSE" pass can still remove the fence if it proves the
    // object doesn't actually escape.
    if (IsInterestingPublishTarget(store_input)) {
      // Merge all constructor fences that we've seen since
      // the last interesting store (or since the beginning).
      MergeCandidateFences();
    }
  }

  bool HasInterestingPublishTargetAsInput(HInstruction* inst) {
    if (candidate_fences_.empty()) {
      // There is no need to look at inputs if there are no candidate fence targets.
      DCHECK_IMPLIES(candidate_fence_targets_.has_value(), candidate_fence_targets_->empty());
      return false;
    }
    for (HInstruction* input : inst->GetInputs()) {
      if (IsInterestingPublishTarget(input)) {
        return true;
      }
    }

    return false;
  }

  // Merges all the existing fences we've seen so far into the last-most fence.
  //
  // This resets the list of candidate fences and their targets back to {}.
  void MergeCandidateFences() {
    if (candidate_fences_.empty()) {
      // Nothing to do, need 1+ fences to merge.
      return;
    }

    // The merge target is always the "last" candidate fence.
    HConstructorFence* merge_target = candidate_fences_.back();
    candidate_fences_.pop_back();

    for (HConstructorFence* fence : candidate_fences_) {
      DCHECK_NE(merge_target, fence);
      merge_target->Merge(fence);
      MaybeRecordStat(stats_, MethodCompilationStat::kConstructorFenceRemovedCFRE);
    }

    if (kCfreLogFenceInputCount) {
      LOG(INFO) << "CFRE-MergeCandidateFences: Post-merge fence input count "
                << merge_target->InputCount();
    }

    // Each merge acts as a cut-off point. The optimization is reset completely.
    // In theory, we could push the fence as far as its publish, but in practice
    // there is no benefit to this extra complexity unless we also reordered
    // the stores to come later.
    candidate_fences_.clear();
    DCHECK(candidate_fence_targets_.has_value());
    candidate_fence_targets_->clear();
  }

  // A publishing 'store' is only interesting if the value being stored
  // is one of the fence `targets` in `candidate_fences`.
  bool IsInterestingPublishTarget(HInstruction* store_input) const {
    DCHECK(candidate_fence_targets_.has_value());
    return candidate_fence_targets_->find(store_input) != candidate_fence_targets_->end();
  }

  // Phase-local heap memory allocator for CFRE optimizer.
  ScopedArenaAllocator scoped_allocator_;

  // Set of constructor fences that we've seen in the current block.
  // Each constructor fences acts as a guard for one or more `targets`.
  // There exist no stores to any `targets` between any of these fences.
  //
  // Fences are in succession order (e.g. fence[i] succeeds fence[i-1]
  // within the same basic block).
  ScopedArenaVector<HConstructorFence*> candidate_fences_;

  // Stores a set of the fence targets, to allow faster lookup of whether
  // a detected publish is a target of one of the candidate fences.
  std::optional<ScopedArenaHashSet<HInstruction*>> candidate_fence_targets_;

  // Used to record stats about the optimization.
  OptimizingCompilerStats* const stats_;

  DISALLOW_COPY_AND_ASSIGN(CFREVisitor);
};

bool ConstructorFenceRedundancyElimination::Run() {
  CFREVisitor cfre_visitor(graph_, stats_);

  // Arbitrarily visit in reverse-post order.
  // The exact block visit order does not matter, as the algorithm
  // only operates on a single block at a time.
  cfre_visitor.VisitReversePostOrder();
  return true;
}

}  // namespace art
