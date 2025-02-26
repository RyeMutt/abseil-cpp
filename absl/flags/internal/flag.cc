//
// Copyright 2019 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/flags/internal/flag.h"

#include "absl/base/optimization.h"
#include "absl/synchronization/mutex.h"

namespace absl {
namespace flags_internal {
namespace {

// Currently we only validate flag values for user-defined flag types.
bool ShouldValidateFlagValue(const CommandLineFlag& flag) {
#define DONT_VALIDATE(T) \
  if (flag.IsOfType<T>()) return false;
  ABSL_FLAGS_INTERNAL_FOR_EACH_LOCK_FREE(DONT_VALIDATE)
  DONT_VALIDATE(std::string)
  DONT_VALIDATE(std::vector<std::string>)
#undef DONT_VALIDATE

  return true;
}

}  // namespace

void FlagImpl::Init() {
  ABSL_CONST_INIT static absl::Mutex init_lock(absl::kConstInit);

  {
    absl::MutexLock lock(&init_lock);

    if (locks_ == nullptr) {  // Must initialize Mutexes for this flag.
      locks_ = new FlagImpl::CommandLineFlagLocks;
    }
  }

  absl::MutexLock lock(&locks_->primary_mu);

  if (def_ != nullptr) {
    inited_.store(true, std::memory_order_release);
  } else {
    // Need to initialize def and cur fields.
    def_ = (*initial_value_gen_)();
    cur_ = Clone(op_, def_);
    StoreAtomic();
    inited_.store(true, std::memory_order_release);
    InvokeCallback();
  }
}

// Ensures that the lazily initialized data is initialized,
// and returns pointer to the mutex guarding flags data.
absl::Mutex* FlagImpl::DataGuard() const
    ABSL_LOCK_RETURNED(locks_->primary_mu) {
  if (ABSL_PREDICT_FALSE(!inited_.load(std::memory_order_acquire))) {
    const_cast<FlagImpl*>(this)->Init();
  }

  // All fields initialized; locks_ is therefore safe to read.
  return &locks_->primary_mu;
}

void FlagImpl::Destroy() const {
  {
    absl::MutexLock l(DataGuard());

    // Values are heap allocated for Abseil Flags.
    if (cur_) Delete(op_, cur_);
    if (def_) Delete(op_, def_);
  }

  delete locks_;
}

std::string FlagImpl::Help() const {
  return help_source_kind_ == FlagHelpSrcKind::kLiteral ? help_.literal
                                                        : help_.gen_func();
}

bool FlagImpl::IsModified() const {
  absl::MutexLock l(DataGuard());
  return modified_;
}

bool FlagImpl::IsSpecifiedOnCommandLine() const {
  absl::MutexLock l(DataGuard());
  return on_command_line_;
}

std::string FlagImpl::DefaultValue() const {
  absl::MutexLock l(DataGuard());

  return Unparse(marshalling_op_, def_);
}

std::string FlagImpl::CurrentValue() const {
  absl::MutexLock l(DataGuard());

  return Unparse(marshalling_op_, cur_);
}

void FlagImpl::SetCallback(
    const flags_internal::FlagCallback mutation_callback) {
  absl::MutexLock l(DataGuard());

  callback_ = mutation_callback;

  InvokeCallback();
}

void FlagImpl::InvokeCallback() const
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(locks_->primary_mu) {
  if (!callback_) return;

  // If the flag has a mutation callback this function invokes it. While the
  // callback is being invoked the primary flag's mutex is unlocked and it is
  // re-locked back after call to callback is completed. Callback invocation is
  // guarded by flag's secondary mutex instead which prevents concurrent
  // callback invocation. Note that it is possible for other thread to grab the
  // primary lock and update flag's value at any time during the callback
  // invocation. This is by design. Callback can get a value of the flag if
  // necessary, but it might be different from the value initiated the callback
  // and it also can be different by the time the callback invocation is
  // completed. Requires that *primary_lock be held in exclusive mode; it may be
  // released and reacquired by the implementation.
  DataGuard()->Unlock();

  {
    absl::MutexLock lock(&locks_->callback_mu);
    callback_();
  }

  DataGuard()->Lock();
}

bool FlagImpl::RestoreState(const CommandLineFlag& flag, const void* value,
                            bool modified, bool on_command_line,
                            int64_t counter) {
  {
    absl::MutexLock l(DataGuard());

    if (counter_ == counter) return false;
  }

  Write(flag, value, op_);

  {
    absl::MutexLock l(DataGuard());

    modified_ = modified;
    on_command_line_ = on_command_line;
  }

  return true;
}

// Attempts to parse supplied `value` string using parsing routine in the `flag`
// argument. If parsing successful, this function stores the parsed value in
// 'dst' assuming it is a pointer to the flag's value type. In case if any error
// is encountered in either step, the error message is stored in 'err'
bool FlagImpl::TryParse(const CommandLineFlag& flag, void* dst,
                        absl::string_view value, std::string* err) const
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(locks_->primary_mu) {
  void* tentative_value = Clone(op_, def_);
  std::string parse_err;
  if (!Parse(marshalling_op_, value, tentative_value, &parse_err)) {
    auto type_name = flag.Typename();
    absl::string_view err_sep = parse_err.empty() ? "" : "; ";
    absl::string_view typename_sep = type_name.empty() ? "" : " ";
    *err = absl::StrCat("Illegal value '", value, "' specified for",
                        typename_sep, type_name, " flag '", flag.Name(), "'",
                        err_sep, parse_err);
    Delete(op_, tentative_value);
    return false;
  }

  Copy(op_, tentative_value, dst);
  Delete(op_, tentative_value);
  return true;
}

void FlagImpl::Read(const CommandLineFlag& flag, void* dst,
                    const flags_internal::FlagOpFn dst_op) const {
  absl::ReaderMutexLock l(DataGuard());

  // `dst_op` is the unmarshaling operation corresponding to the declaration
  // visibile at the call site. `op` is the Flag's defined unmarshalling
  // operation. They must match for this operation to be well-defined.
  if (ABSL_PREDICT_FALSE(dst_op != op_)) {
    ABSL_INTERNAL_LOG(
        ERROR,
        absl::StrCat("Flag '", flag.Name(),
                     "' is defined as one type and declared as another"));
  }
  CopyConstruct(op_, cur_, dst);
}

void FlagImpl::StoreAtomic() ABSL_EXCLUSIVE_LOCKS_REQUIRED(locks_->primary_mu) {
  size_t data_size = Sizeof(op_);

  if (data_size <= sizeof(int64_t)) {
    int64_t t = 0;
    std::memcpy(&t, cur_, data_size);
    atomic_.store(t, std::memory_order_release);
  }
}

void FlagImpl::Write(const CommandLineFlag& flag, const void* src,
                     const flags_internal::FlagOpFn src_op) {
  absl::MutexLock l(DataGuard());

  // `src_op` is the marshalling operation corresponding to the declaration
  // visible at the call site. `op` is the Flag's defined marshalling operation.
  // They must match for this operation to be well-defined.
  if (ABSL_PREDICT_FALSE(src_op != op_)) {
    ABSL_INTERNAL_LOG(
        ERROR,
        absl::StrCat("Flag '", flag.Name(),
                     "' is defined as one type and declared as another"));
  }

  if (ShouldValidateFlagValue(flag)) {
    void* obj = Clone(op_, src);
    std::string ignored_error;
    std::string src_as_str = Unparse(marshalling_op_, src);
    if (!Parse(marshalling_op_, src_as_str, obj, &ignored_error)) {
      ABSL_INTERNAL_LOG(ERROR,
                        absl::StrCat("Attempt to set flag '", flag.Name(),
                                     "' to invalid value ", src_as_str));
    }
    Delete(op_, obj);
  }

  modified_ = true;
  counter_++;
  Copy(op_, src, cur_);

  StoreAtomic();
  InvokeCallback();
}

// Sets the value of the flag based on specified string `value`. If the flag
// was successfully set to new value, it returns true. Otherwise, sets `err`
// to indicate the error, leaves the flag unchanged, and returns false. There
// are three ways to set the flag's value:
//  * Update the current flag value
//  * Update the flag's default value
//  * Update the current flag value if it was never set before
// The mode is selected based on 'set_mode' parameter.
bool FlagImpl::SetFromString(const CommandLineFlag& flag,
                             absl::string_view value, FlagSettingMode set_mode,
                             ValueSource source, std::string* err) {
  absl::MutexLock l(DataGuard());

  switch (set_mode) {
    case SET_FLAGS_VALUE: {
      // set or modify the flag's value
      if (!TryParse(flag, cur_, value, err)) return false;
      modified_ = true;
      counter_++;
      StoreAtomic();
      InvokeCallback();

      if (source == kCommandLine) {
        on_command_line_ = true;
      }
      break;
    }
    case SET_FLAG_IF_DEFAULT: {
      // set the flag's value, but only if it hasn't been set by someone else
      if (!modified_) {
        if (!TryParse(flag, cur_, value, err)) return false;
        modified_ = true;
        counter_++;
        StoreAtomic();
        InvokeCallback();
      } else {
        // TODO(rogeeff): review and fix this semantic. Currently we do not fail
        // in this case if flag is modified. This is misleading since the flag's
        // value is not updated even though we return true.
        // *err = absl::StrCat(Name(), " is already set to ",
        //                     CurrentValue(), "\n");
        // return false;
        return true;
      }
      break;
    }
    case SET_FLAGS_DEFAULT: {
      // modify the flag's default-value
      if (!TryParse(flag, def_, value, err)) return false;

      if (!modified_) {
        // Need to set both default value *and* current, in this case
        Copy(op_, def_, cur_);
        StoreAtomic();
        InvokeCallback();
      }
      break;
    }
  }

  return true;
}

void FlagImpl::CheckDefaultValueParsingRoundtrip(
    const CommandLineFlag& flag) const {
  std::string v = DefaultValue();

  absl::MutexLock lock(DataGuard());

  void* dst = Clone(op_, def_);
  std::string error;
  if (!flags_internal::Parse(marshalling_op_, v, dst, &error)) {
    ABSL_INTERNAL_LOG(
        FATAL,
        absl::StrCat("Flag ", flag.Name(), " (from ", flag.Filename(),
                     "): std::string form of default value '", v,
                     "' could not be parsed; error=", error));
  }

  // We do not compare dst to def since parsing/unparsing may make
  // small changes, e.g., precision loss for floating point types.
  Delete(op_, dst);
}

bool FlagImpl::ValidateInputValue(absl::string_view value) const {
  absl::MutexLock l(DataGuard());

  void* obj = Clone(op_, def_);
  std::string ignored_error;
  const bool result =
      flags_internal::Parse(marshalling_op_, value, obj, &ignored_error);
  Delete(op_, obj);
  return result;
}

}  // namespace flags_internal
}  // namespace absl
