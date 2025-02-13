/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_INTRINSICS_ARM_VIXL_H_
#define ART_COMPILER_OPTIMIZING_INTRINSICS_ARM_VIXL_H_

#include "base/macros.h"
#include "intrinsics.h"
#include "utils/arm/assembler_arm_vixl.h"

namespace art HIDDEN {

namespace arm {

class ArmVIXLAssembler;
class CodeGeneratorARMVIXL;

class IntrinsicLocationsBuilderARMVIXL final : public IntrinsicVisitor {
 public:
  explicit IntrinsicLocationsBuilderARMVIXL(CodeGeneratorARMVIXL* codegen);

  // Define visitor methods.

#define OPTIMIZING_INTRINSICS(Name, IsStatic, NeedsEnvironmentOrCache, SideEffects, Exceptions, ...) \
  void Visit ## Name(HInvoke* invoke) override;
#include "intrinsics_list.h"
  INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS

  // Check whether an invoke is an intrinsic, and if so, create a location summary. Returns whether
  // a corresponding LocationSummary with the intrinsified_ flag set was generated and attached to
  // the invoke.
  bool TryDispatch(HInvoke* invoke);

 private:
  ArenaAllocator* const allocator_;
  CodeGeneratorARMVIXL* const codegen_;
  ArmVIXLAssembler* const assembler_;
  const ArmInstructionSetFeatures& features_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicLocationsBuilderARMVIXL);
};

class IntrinsicCodeGeneratorARMVIXL final : public IntrinsicVisitor {
 public:
  explicit IntrinsicCodeGeneratorARMVIXL(CodeGeneratorARMVIXL* codegen) : codegen_(codegen) {}

  // Define visitor methods.

#define OPTIMIZING_INTRINSICS(Name, IsStatic, NeedsEnvironmentOrCache, SideEffects, Exceptions, ...) \
  void Visit ## Name(HInvoke* invoke) override;
#include "intrinsics_list.h"
  INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS

 private:
  ArenaAllocator* GetAllocator();
  ArmVIXLAssembler* GetAssembler();

  void HandleValueOf(HInvoke* invoke,
                     const IntrinsicVisitor::ValueOfInfo& info,
                     DataType::Type primitive_type);

  CodeGeneratorARMVIXL* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicCodeGeneratorARMVIXL);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_ARM_VIXL_H_
