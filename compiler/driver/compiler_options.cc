/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "compiler_options.h"

#include <fstream>
#include <string_view>

#include "android-base/stringprintf.h"

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "art_method-inl.h"
#include "base/runtime_debug.h"
#include "base/string_view_cpp20.h"
#include "base/variant_map.h"
#include "class_linker.h"
#include "cmdline_parser.h"
#include "compiler_options_map-inl.h"
#include "dex/dex_file-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "simple_compiler_options_map.h"

namespace art HIDDEN {

CompilerOptions::CompilerOptions()
    : compiler_filter_(CompilerFilter::kDefaultCompilerFilter),
      huge_method_threshold_(kDefaultHugeMethodThreshold),
      inline_max_code_units_(kUnsetInlineMaxCodeUnits),
      instruction_set_(kRuntimeISA == InstructionSet::kArm ? InstructionSet::kThumb2 : kRuntimeISA),
      instruction_set_features_(nullptr),
      no_inline_from_(),
      dex_files_for_oat_file_(),
      image_classes_(),
      compiler_type_(CompilerType::kAotCompiler),
      image_type_(ImageType::kNone),
      multi_image_(false),
      compile_art_test_(false),
      baseline_(false),
      debuggable_(false),
      generate_debug_info_(false),
      generate_mini_debug_info_(true),
      generate_build_id_(false),
      implicit_null_checks_(true),
      implicit_so_checks_(true),
      implicit_suspend_checks_(false),
      compile_pic_(false),
      dump_timings_(false),
      dump_pass_timings_(false),
      dump_stats_(false),
      profile_branches_(false),
      profile_compilation_info_(nullptr),
      verbose_methods_(),
      abort_on_hard_verifier_failure_(false),
      abort_on_soft_verifier_failure_(false),
      init_failure_output_(nullptr),
      dump_cfg_file_name_(""),
      dump_cfg_append_(false),
      force_determinism_(false),
      check_linkage_conditions_(false),
      crash_on_linkage_violation_(false),
      deduplicate_code_(true),
      count_hotness_in_compiled_code_(false),
      resolve_startup_const_strings_(false),
      initialize_app_image_classes_(false),
      check_profiled_methods_(ProfileMethodsCheck::kNone),
      max_image_block_size_(std::numeric_limits<uint32_t>::max()),
      register_allocation_strategy_(RegisterAllocator::kRegisterAllocatorDefault),
      passes_to_run_(nullptr) {
}

CompilerOptions::~CompilerOptions() {
  // Everything done by member destructors.
  // The definitions of classes forward-declared in the header have now been #included.
}

namespace {

bool kEmitRuntimeReadBarrierChecks = kIsDebugBuild &&
    RegisterRuntimeDebugFlag(&kEmitRuntimeReadBarrierChecks);

}  // namespace

bool CompilerOptions::EmitRunTimeChecksInDebugMode() const {
  // Run-time checks (e.g. Marking Register checks) are only emitted in slow-debug mode.
  return kEmitRuntimeReadBarrierChecks;
}

bool CompilerOptions::ParseDumpInitFailures(const std::string& option, std::string* error_msg) {
  init_failure_output_.reset(new std::ofstream(option));
  if (init_failure_output_.get() == nullptr) {
    *error_msg = "Failed to construct std::ofstream";
    return false;
  } else if (init_failure_output_->fail()) {
    *error_msg = android::base::StringPrintf(
        "Failed to open %s for writing the initialization failures.", option.c_str());
    init_failure_output_.reset();
    return false;
  }
  return true;
}

bool CompilerOptions::ParseRegisterAllocationStrategy(const std::string& option,
                                                      std::string* error_msg) {
  if (option == "linear-scan") {
    register_allocation_strategy_ = RegisterAllocator::Strategy::kRegisterAllocatorLinearScan;
  } else if (option == "graph-color") {
    register_allocation_strategy_ = RegisterAllocator::Strategy::kRegisterAllocatorGraphColor;
  } else {
    *error_msg = "Unrecognized register allocation strategy. Try linear-scan, or graph-color.";
    return false;
  }
  return true;
}

bool CompilerOptions::ParseCompilerOptions(const std::vector<std::string>& options,
                                           bool ignore_unrecognized,
                                           std::string* error_msg) {
  auto parser = CreateSimpleParser(ignore_unrecognized);
  CmdlineResult parse_result = parser.Parse(options);
  if (!parse_result.IsSuccess()) {
    *error_msg = parse_result.GetMessage();
    return false;
  }

  SimpleParseArgumentMap args = parser.ReleaseArgumentsMap();
  return ReadCompilerOptions(args, this, error_msg);
}

bool CompilerOptions::IsImageClass(const char* descriptor) const {
  // Historical note: We used to hold the set indirectly and there was a distinction between an
  // empty set and a null, null meaning to include all classes. However, the distinction has been
  // removed; if we don't have a profile, we treat it as an empty set of classes. b/77340429
  return image_classes_.find(std::string_view(descriptor)) != image_classes_.end();
}

bool CompilerOptions::IsPreloadedClass(const char* pretty_descriptor) const {
  return preloaded_classes_.find(std::string_view(pretty_descriptor)) != preloaded_classes_.end();
}

bool CompilerOptions::ShouldCompileWithClinitCheck(ArtMethod* method) const {
  if (method != nullptr &&
      Runtime::Current()->IsAotCompiler() &&
      method->IsStatic() &&
      !method->IsConstructor() &&
      // Compiled code for native methods never do a clinit check, so we may put the resolution
      // trampoline for native methods. This means that it's possible post zygote fork for the
      // entry to be dirtied. We could resolve this by either:
      // - Make these methods use the generic JNI entrypoint, but that's not
      //   desirable for a method that is in the profile.
      // - Ensure the declaring class of such native methods are always in the
      //   preloaded-classes list.
      // - Emit the clinit check in the compiled code of native methods.
      !method->IsNative()) {
    ScopedObjectAccess soa(Thread::Current());
    ObjPtr<mirror::Class> cls = method->GetDeclaringClass<kWithoutReadBarrier>();
    return cls->IsInBootImageAndNotInPreloadedClasses();
  }
  return false;
}

}  // namespace art
