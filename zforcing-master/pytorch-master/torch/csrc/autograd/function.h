#pragma once

// Function is an abstract class that represents a single operation from one or
// more variables to one more or varaibles.
//
// Subclasses may represent "forward" or "backward" operations (i.e functions
// and their derivatives). Some functions may be used as both.

#include <memory>
#include <THPP/THPP.h>
#include <vector>

#include "torch/csrc/autograd/function_hook.h"

namespace torch { namespace autograd {

struct Function;
struct Variable;

using tensor_list = std::vector<std::unique_ptr<thpp::Tensor>>;
using variable_list = std::vector<std::shared_ptr<Variable>>;
using function_list = std::vector<std::pair<std::shared_ptr<Function>, int>>;

// State used to create "backward" functions
struct FunctionFlags {
  bool is_executable = false;
  bool is_volatile = false;
  function_list next_functions;
};

struct Function {
  Function()
    : num_inputs(0)
    , next_functions()
    , is_executable(false)
    , is_stochastic(false)
    , pre_hooks()
    , post_hooks()
    {}

  Function(FunctionFlags&& flags)
    : num_inputs(0)
    , next_functions(std::move(flags.next_functions))
    , is_executable(flags.is_executable)
    , is_stochastic(false)
    , pre_hooks()
    , post_hooks()
    {}

  Function(const Function& other) = delete;
  Function(Function&& other) = delete;
  virtual ~Function() {}

  // Implements the operation
  virtual variable_list apply(const variable_list& inputs) = 0;

  // Computes is_executable, is_volatile, and next_functions from a list
  // of input variables
  static FunctionFlags flags(const variable_list& inputs);

  // Releases saved variables if the operation won't be reused
  virtual inline void releaseVariables() {}

  // Function name for debugging
  virtual std::string name();

  inline bool should_compute_output(int i) const {
    auto& fn = next_functions[i].first;
    return fn && fn->is_executable;
  }

  inline void set_flags(FunctionFlags&& flags) {
    is_executable = flags.is_executable;
    next_functions = std::move(flags.next_functions);
  }

  int num_inputs;
  function_list next_functions;
  bool is_executable;
  bool is_stochastic;
  std::vector<std::shared_ptr<FunctionPreHook>> pre_hooks;
  std::vector<std::shared_ptr<FunctionPostHook>> post_hooks;
};


}} // namespace torch::autograd
