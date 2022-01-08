#include "torch/csrc/autograd/variable.h"

#include "torch/csrc/autograd/functions/accumulate_grad.h"
#include "torch/csrc/utils/auto_gpu.h"

using namespace torch;
using namespace thpp;

namespace torch { namespace autograd {

Variable::Variable(
  std::unique_ptr<thpp::Tensor> data,
  bool requires_grad,
  bool is_volatile)
    : data(std::move(data))
    , grad_fn(nullptr)
    , grad(nullptr)
    , version_counter(new VariableVersion())
    , requires_grad(requires_grad)
    , is_volatile(is_volatile)
    , output_nr(0)
    , pyobj(nullptr)
{
  if (!this->data) {
    throw std::runtime_error("Variable data is NULL");
  }
}

Variable::Variable(
  std::unique_ptr<thpp::Tensor> data,
  std::shared_ptr<Function> grad_fn)
    : data(std::move(data))
    , grad_fn(grad_fn)
    , grad(nullptr)
    , version_counter(new VariableVersion())
    , requires_grad(grad_fn->is_executable)
    , is_volatile(false)
    , output_nr(grad_fn->num_inputs++)
    , pyobj(nullptr)
{
  if (!this->data) {
    throw std::runtime_error("Variable data is NULL");
  }
}

auto Variable::get_grad_accumulator() -> std::shared_ptr<Function> {
  using weak_type = std::weak_ptr<Function>;

  static std::shared_ptr<Function> null_shared_ptr;
  static weak_type null_weak_ptr;

  if (grad_fn) return nullptr;
  if (!requires_grad) return nullptr;

  auto result = grad_accumulator.lock();
  if (result) return result;

  // That didn't work, we need to allocate it, but taking into account that other
  // threads might be doing the same thing.
  std::lock_guard<std::mutex> lock(grad_accumulator_lock);

  result = grad_accumulator.lock();
  if (result) return result;

  result = std::make_shared<AccumulateGrad>(shared_from_this());
  grad_accumulator = result;
  return result;
}

auto SavedVariable::unpack() -> std::shared_ptr<Variable> {
  if (!data) return nullptr;

  int current_version = **version;
  if (expected_version != current_version) {
    throw std::runtime_error("one of the variables "
        "needed for gradient computation has been modified by an "
        "inplace operation");
  }

  auto new_var = std::make_shared<Variable>(
      std::unique_ptr<thpp::Tensor>(data->clone_shallow()),
      requires_grad, is_volatile);
  if (!grad_fn && !weak_grad_fn.expired()) {
    // there's no risk of race condition here, because weak_grad_fn is
    // guaranteed to be valid for the entire duration of the call
    // (of course only if it was used in the first place).
    new_var->grad_fn = weak_grad_fn.lock();
  } else {
    new_var->grad_fn = grad_fn;
  }
  new_var->version_counter->join_with(*version);
  // If a Variable is a leaf (no grad_fn saved), and it requires_grad, then we
  // should have saved the grad accumulator. Even if the Variable no longer
  // alive, the accumulator should be kept alive by the references in the graph).
  if (requires_grad && !grad_fn && weak_grad_fn.expired() && grad_accumulator.expired())
    throw std::logic_error("No grad accumulator for a saved leaf!");
  new_var->grad_accumulator = grad_accumulator;

  return new_var;
}

}} // namespace torch::autograd
