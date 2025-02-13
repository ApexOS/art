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

#ifndef ART_COMPILER_OPTIMIZING_CODEGEN_TEST_UTILS_H_
#define ART_COMPILER_OPTIMIZING_CODEGEN_TEST_UTILS_H_

#include "arch/arm/registers_arm.h"
#include "arch/instruction_set.h"
#include "arch/x86/registers_x86.h"
#include "base/macros.h"
#include "code_simulator.h"
#include "code_simulator_container.h"
#include "common_compiler_test.h"
#include "graph_checker.h"
#include "prepare_for_register_allocation.h"
#include "ssa_liveness_analysis.h"

#ifdef ART_ENABLE_CODEGEN_arm
#include "code_generator_arm_vixl.h"
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
#include "code_generator_arm64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_riscv64
#include "code_generator_riscv64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86
#include "code_generator_x86.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86_64
#include "code_generator_x86_64.h"
#endif

namespace art HIDDEN {

using CreateCodegenFn = CodeGenerator* (*)(HGraph*, const CompilerOptions&);

class CodegenTargetConfig {
 public:
  CodegenTargetConfig(InstructionSet isa, CreateCodegenFn create_codegen)
      : isa_(isa), create_codegen_(create_codegen) {
  }
  InstructionSet GetInstructionSet() const { return isa_; }
  CodeGenerator* CreateCodeGenerator(HGraph* graph, const CompilerOptions& compiler_options) {
    return create_codegen_(graph, compiler_options);
  }

 private:
  InstructionSet isa_;
  CreateCodegenFn create_codegen_;
};

#ifdef ART_ENABLE_CODEGEN_arm
// Special ARM code generator for codegen testing in a limited code
// generation environment (i.e. with no runtime support).
//
// Note: If we want to exercise certains HIR constructions
// (e.g. reference field load in Baker read barrier configuration) in
// codegen tests in the future, we should also:
// - save the Thread Register (R9) and possibly the Marking Register
//   (R8) before entering the generated function (both registers are
//   callee-save in AAPCS);
// - set these registers to meaningful values before or upon entering
//   the generated function (so that generated code using them is
//   correct);
// - restore their original values before leaving the generated
//   function.

// Provide our own codegen, that ensures the C calling conventions
// are preserved. Currently, ART and C do not match as R4 is caller-save
// in ART, and callee-save in C. Alternatively, we could use or write
// the stub that saves and restores all registers, but it is easier
// to just overwrite the code generator.
class TestCodeGeneratorARMVIXL : public arm::CodeGeneratorARMVIXL {
 public:
  TestCodeGeneratorARMVIXL(HGraph* graph, const CompilerOptions& compiler_options)
      : arm::CodeGeneratorARMVIXL(graph, compiler_options) {
    AddAllocatedRegister(Location::RegisterLocation(arm::R6));
    AddAllocatedRegister(Location::RegisterLocation(arm::R7));
  }

  void SetupBlockedRegisters() const override {
    arm::CodeGeneratorARMVIXL::SetupBlockedRegisters();
    blocked_core_registers_[arm::R4] = true;
    blocked_core_registers_[arm::R6] = false;
    blocked_core_registers_[arm::R7] = false;
  }

  void MaybeGenerateMarkingRegisterCheck(int code ATTRIBUTE_UNUSED,
                                         Location temp_loc ATTRIBUTE_UNUSED) override {
    // When turned on, the marking register checks in
    // CodeGeneratorARMVIXL::MaybeGenerateMarkingRegisterCheck expects the
    // Thread Register and the Marking Register to be set to
    // meaningful values. This is not the case in codegen testing, so
    // just disable them entirely here (by doing nothing in this
    // method).
  }
};
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
// Special ARM64 code generator for codegen testing in a limited code
// generation environment (i.e. with no runtime support).
//
// Note: If we want to exercise certains HIR constructions
// (e.g. reference field load in Baker read barrier configuration) in
// codegen tests in the future, we should also:
// - save the Thread Register (X19) and possibly the Marking Register
//   (X20) before entering the generated function (both registers are
//   callee-save in AAPCS64);
// - set these registers to meaningful values before or upon entering
//   the generated function (so that generated code using them is
//   correct);
// - restore their original values before leaving the generated
//   function.
class TestCodeGeneratorARM64 : public arm64::CodeGeneratorARM64 {
 public:
  TestCodeGeneratorARM64(HGraph* graph, const CompilerOptions& compiler_options)
      : arm64::CodeGeneratorARM64(graph, compiler_options) {}

  void MaybeGenerateMarkingRegisterCheck(int codem ATTRIBUTE_UNUSED,
                                         Location temp_loc ATTRIBUTE_UNUSED) override {
    // When turned on, the marking register checks in
    // CodeGeneratorARM64::MaybeGenerateMarkingRegisterCheck expect the
    // Thread Register and the Marking Register to be set to
    // meaningful values. This is not the case in codegen testing, so
    // just disable them entirely here (by doing nothing in this
    // method).
  }
};
#endif

#ifdef ART_ENABLE_CODEGEN_x86
class TestCodeGeneratorX86 : public x86::CodeGeneratorX86 {
 public:
  TestCodeGeneratorX86(HGraph* graph, const CompilerOptions& compiler_options)
      : x86::CodeGeneratorX86(graph, compiler_options) {
    // Save edi, we need it for getting enough registers for long multiplication.
    AddAllocatedRegister(Location::RegisterLocation(x86::EDI));
  }

  void SetupBlockedRegisters() const override {
    x86::CodeGeneratorX86::SetupBlockedRegisters();
    // ebx is a callee-save register in C, but caller-save for ART.
    blocked_core_registers_[x86::EBX] = true;

    // Make edi available.
    blocked_core_registers_[x86::EDI] = false;
  }
};
#endif

class InternalCodeAllocator : public CodeAllocator {
 public:
  InternalCodeAllocator() : size_(0) { }

  uint8_t* Allocate(size_t size) override {
    size_ = size;
    memory_.reset(new uint8_t[size]);
    return memory_.get();
  }

  size_t GetSize() const { return size_; }
  ArrayRef<const uint8_t> GetMemory() const override {
    return ArrayRef<const uint8_t>(memory_.get(), size_);
  }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> memory_;

  DISALLOW_COPY_AND_ASSIGN(InternalCodeAllocator);
};

static bool CanExecuteOnHardware(InstructionSet target_isa) {
  return (target_isa == kRuntimeISA)
      // Handle the special case of ARM, with two instructions sets (ARM32 and Thumb-2).
      || (kRuntimeISA == InstructionSet::kArm && target_isa == InstructionSet::kThumb2);
}

static bool CanExecute(InstructionSet target_isa) {
  CodeSimulatorContainer simulator(target_isa);
  return CanExecuteOnHardware(target_isa) || simulator.CanSimulate();
}

template <typename Expected>
inline static Expected SimulatorExecute(CodeSimulator* simulator, Expected (*f)());

template <>
inline bool SimulatorExecute<bool>(CodeSimulator* simulator, bool (*f)()) {
  simulator->RunFrom(reinterpret_cast<intptr_t>(f));
  return simulator->GetCReturnBool();
}

template <>
inline int32_t SimulatorExecute<int32_t>(CodeSimulator* simulator, int32_t (*f)()) {
  simulator->RunFrom(reinterpret_cast<intptr_t>(f));
  return simulator->GetCReturnInt32();
}

template <>
inline int64_t SimulatorExecute<int64_t>(CodeSimulator* simulator, int64_t (*f)()) {
  simulator->RunFrom(reinterpret_cast<intptr_t>(f));
  return simulator->GetCReturnInt64();
}

template <typename Expected>
static void VerifyGeneratedCode(InstructionSet target_isa,
                                Expected (*f)(),
                                bool has_result,
                                Expected&& expected) {
  ASSERT_TRUE(CanExecute(target_isa)) << "Target isa is not executable.";

  // Verify on simulator.
  CodeSimulatorContainer simulator(target_isa);
  if (simulator.CanSimulate()) {
    Expected result = SimulatorExecute<Expected>(simulator.Get(), f);
    if (has_result) {
      ASSERT_EQ(expected, result);
    }
  }

  // Verify on hardware.
  if (CanExecuteOnHardware(target_isa)) {
    Expected result = f();
    if (has_result) {
      ASSERT_EQ(expected, result);
    }
  }
}

template <typename Expected>
static void Run(const InternalCodeAllocator& allocator,
                const CodeGenerator& codegen,
                bool has_result,
                Expected&& expected) {
  InstructionSet target_isa = codegen.GetInstructionSet();

  struct CodeHolder : CommonCompilerTestImpl {
   protected:
    ClassLinker* GetClassLinker() override { return nullptr; }
    Runtime* GetRuntime() override { return nullptr; }
  };
  CodeHolder code_holder;
  const void* method_code =
      code_holder.MakeExecutable(allocator.GetMemory(), ArrayRef<const uint8_t>(), target_isa);

  using fptr = Expected (*)();
  fptr f = reinterpret_cast<fptr>(reinterpret_cast<uintptr_t>(method_code));
  VerifyGeneratedCode(target_isa, f, has_result, expected);
}

static void ValidateGraph(HGraph* graph) {
  GraphChecker graph_checker(graph);
  graph_checker.Run();
  if (!graph_checker.IsValid()) {
    for (const std::string& error : graph_checker.GetErrors()) {
      std::cout << error << std::endl;
    }
  }
  ASSERT_TRUE(graph_checker.IsValid());
}

template <typename Expected>
static void RunCodeNoValidation(CodeGenerator* codegen,
                                HGraph* graph,
                                const std::function<void(HGraph*)>& hook_before_codegen,
                                bool has_result,
                                Expected&& expected) {
  {
    ScopedArenaAllocator local_allocator(graph->GetArenaStack());
    SsaLivenessAnalysis liveness(graph, codegen, &local_allocator);
    PrepareForRegisterAllocation(graph, codegen->GetCompilerOptions()).Run();
    liveness.Analyze();
    std::unique_ptr<RegisterAllocator> register_allocator =
        RegisterAllocator::Create(&local_allocator, codegen, liveness);
    register_allocator->AllocateRegisters();
  }
  hook_before_codegen(graph);
  InternalCodeAllocator allocator;
  const bool result = codegen->Compile();
  CHECK(result);
  Run(allocator, *codegen, has_result, expected);
}

template <typename Expected>
static void RunCode(CodeGenerator* codegen,
                    HGraph* graph,
                    std::function<void(HGraph*)> hook_before_codegen,
                    bool has_result,
                    Expected&& expected) {
  ValidateGraph(graph);
  RunCodeNoValidation(codegen, graph, hook_before_codegen, has_result, expected);
}

template <typename Expected>
static void RunCode(CodegenTargetConfig target_config,
                    const CompilerOptions& compiler_options,
                    HGraph* graph,
                    std::function<void(HGraph*)> hook_before_codegen,
                    bool has_result,
                    Expected&& expected) {
  std::unique_ptr<CodeGenerator> codegen(target_config.CreateCodeGenerator(graph,
                                                                           compiler_options));
  RunCode(codegen.get(), graph, hook_before_codegen, has_result, expected);
}

#ifdef ART_ENABLE_CODEGEN_arm
inline CodeGenerator* create_codegen_arm_vixl32(HGraph* graph, const CompilerOptions& compiler_options) {
  return new (graph->GetAllocator()) TestCodeGeneratorARMVIXL(graph, compiler_options);
}
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
inline CodeGenerator* create_codegen_arm64(HGraph* graph, const CompilerOptions& compiler_options) {
  return new (graph->GetAllocator()) TestCodeGeneratorARM64(graph, compiler_options);
}
#endif

#ifdef ART_ENABLE_CODEGEN_riscv64
inline CodeGenerator* create_codegen_riscv64(HGraph*, const CompilerOptions&) { return nullptr; }
#endif

#ifdef ART_ENABLE_CODEGEN_x86
inline CodeGenerator* create_codegen_x86(HGraph* graph, const CompilerOptions& compiler_options) {
  return new (graph->GetAllocator()) TestCodeGeneratorX86(graph, compiler_options);
}
#endif

#ifdef ART_ENABLE_CODEGEN_x86_64
inline CodeGenerator* create_codegen_x86_64(HGraph* graph, const CompilerOptions& compiler_options) {
  return new (graph->GetAllocator()) x86_64::CodeGeneratorX86_64(graph, compiler_options);
}
#endif

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODEGEN_TEST_UTILS_H_
