#include "torch/csrc/autograd/engine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <THPP/THPP.h>
#include <thread>
#include <unordered_set>
#include <typeinfo>
#include <sstream>

#ifdef WITH_CUDA
#include <cuda.h>
#include <THC/THC.h>
#endif

using thpp::Tensor;

namespace torch { namespace autograd {

// XXX: Changes to the way multithreading works in execute should be done with
// great care. Right now the implementation guarantees that a single function's
// apply will never be entered concurrently (even if multiple graphs are
// executed at the same time). Adding multiple threads per-device or removing
// engine thread affinity to the device can break this invariant, and we depend
// on it in a few places (e.g. AccumulateGrad function).

struct FunctionTask {
  GraphTask* base;
  std::shared_ptr<Function> fn;
  InputBuffer inputs;

  FunctionTask(GraphTask* base, std::shared_ptr<Function> fn, InputBuffer inputs)
    : base(base)
    , fn(fn)
    , inputs(std::move(inputs)) {}
};

struct ReadyQueue {
  std::deque<FunctionTask> queue;
  std::condition_variable not_empty;
  std::mutex mutex;

  void push_front(FunctionTask item);
  FunctionTask pop_back();
};

struct GraphTask {
  std::exception_ptr exception;
  std::atomic_bool has_error;
  std::atomic<uint64_t> outstanding_tasks;
  bool keep_graph;
  bool has_any_work;

  std::mutex mutex;
  std::condition_variable not_done;
  const Engine::callback_map& function_callbacks;
  std::unordered_map<Function*, InputBuffer> not_ready;
  std::unordered_map<Function*, int> dependencies;

  GraphTask(bool keep_graph, const Engine::callback_map& function_callbacks)
    : exception()
    , has_error(false)
    , outstanding_tasks(0)
    , keep_graph(keep_graph)
    , has_any_work(false)
    , mutex()
    , not_done()
    , function_callbacks(function_callbacks)
    , not_ready()
    , dependencies() {}
};

auto ReadyQueue::push_front(FunctionTask item) -> void {
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++item.base->outstanding_tasks;
    queue.push_front(std::move(item));
  }
  not_empty.notify_one();
}

auto ReadyQueue::pop_back() -> FunctionTask {
  std::unique_lock<std::mutex> lock(mutex);
  not_empty.wait(lock, [this]{ return !queue.empty(); });
  auto task = std::move(queue.back()); queue.pop_back();
  return task;
}

Engine::Engine() : ready_queues() {
}

// This Engine's ReadyQueues and their corresponding threads are leaked here
Engine::~Engine() = default;

auto Engine::thread_main(std::shared_ptr<ReadyQueue> queue) -> void {
  while (1) {
    FunctionTask task = queue->pop_back();
    if (!task.base->has_error.load()) {
      try {
        evaluate_function(task);
      } catch (std::exception& e) {
        thread_on_exception(task, e);
      }
    }
    if (--task.base->outstanding_tasks == 0) {
      std::lock_guard<std::mutex> lock(task.base->mutex);
      task.base->not_done.notify_all();
    }
  }
}

auto Engine::thread_on_exception(FunctionTask& task, std::exception& e) -> void {
  std::lock_guard<std::mutex> lock(task.base->mutex);
  if (!task.base->has_error.load()) {
    task.base->exception = std::current_exception();
    task.base->has_error = true;
  }
}

static variable_list call_pre_hooks(Function& fn, variable_list inputs) {
  for (auto& hook : fn.pre_hooks) {
    inputs = (*hook)(inputs);
  }
  return inputs;
}

static variable_list call_post_hooks(Function& fn, variable_list outputs, variable_list inputs) {
  for (auto& hook : fn.post_hooks) {
    outputs = (*hook)(outputs, inputs);
  }
  return outputs;
}

static variable_list call_function(FunctionTask& task) {
  auto& fn = *task.fn;
  auto inputs = call_pre_hooks(fn, InputBuffer::variables(std::move(task.inputs)));

  auto& function_callbacks = task.base->function_callbacks;
  auto callback_it = function_callbacks.find(&fn);
  if (callback_it != function_callbacks.end()) {
    auto& callback = callback_it->second;
    if (!callback(&fn, inputs)) return variable_list(fn.next_functions.size());
  }

  auto fn_outputs = fn.apply(inputs);
  return call_post_hooks(fn, std::move(fn_outputs), std::move(inputs));
}

auto Engine::evaluate_function(FunctionTask& task) -> void {
  auto outputs = call_function(task);

  auto& fn = *task.fn;
  if (!task.base->keep_graph) {
    fn.releaseVariables();
  }

  if (outputs.size() != fn.next_functions.size()) {
    std::stringstream ss;
    ss << "Function '" << fn.name() << "' returned an invalid number of outputs - expected ";
    ss << fn.next_functions.size() << ", but got " << outputs.size();
    throw std::runtime_error(ss.str());
  }

  int num_outputs = outputs.size();
  for (int i = 0; i < num_outputs; ++i) {
    auto& output = outputs[i];
    auto& next_fn = fn.next_functions[i].first;
    int input_nr = fn.next_functions[i].second;

    if (!next_fn) {
      continue;
    }

    // Stochastic functions are placed in the ready queue by
    // compute_dependencies, so we have to skip them here.
    if (next_fn->is_stochastic || !next_fn->is_executable) {
      continue;
    }

    std::lock_guard<std::mutex> lock(task.base->mutex);
    // Check if the next function is ready to be computed
    bool is_ready = false;
    auto& dependencies = task.base->dependencies;
    auto it = dependencies.find(next_fn.get());
    if (it == dependencies.end()) {
      auto name = next_fn->name();
      throw std::runtime_error(std::string("dependency not found for ") + name);
    } else if (--it->second == 0) {
      dependencies.erase(it);
      is_ready = true;
    }

    auto& not_ready = task.base->not_ready;
    auto not_ready_it = not_ready.find(next_fn.get());
    if (not_ready_it == not_ready.end()) {
      // No buffers have been allocated for the function
      InputBuffer input_buffer(next_fn->num_inputs);
      input_buffer.add(input_nr, std::move(output));
      if (is_ready) {
        auto& queue = ready_queue(input_buffer.device());
        queue.push_front(FunctionTask(task.base, next_fn, std::move(input_buffer)));
      } else {
        not_ready.emplace(next_fn.get(), std::move(input_buffer));
      }
    } else {
      // The function already has a buffer
      auto &input_buffer = not_ready_it->second;
      input_buffer.add(input_nr, std::move(output));
      if (is_ready) {
        auto& queue = ready_queue(input_buffer.device());
        queue.push_front(FunctionTask(task.base, next_fn, std::move(input_buffer)));
        not_ready.erase(not_ready_it);
      }
    }
  }
}

/** Finds all stochastic functions and appends them to the queue */
auto Engine::find_stochastic_functions(function_queue& queue, GraphTask& task) -> void {
  std::unordered_set<Function*> seen;
  function_queue search_queue(queue);
  while (search_queue.size() > 0) {
    auto fn = search_queue.back(); search_queue.pop_back();
    for (auto& next_fn_pair : fn->next_functions) {
      auto& next_fn = next_fn_pair.first;
      Function* next_ptr = next_fn.get();
      if (!next_ptr) continue;
      if (next_ptr->is_stochastic && next_ptr->is_executable && seen.count(next_ptr) == 0) {
        ready_queue(-1).push_front(FunctionTask(&task, next_fn, InputBuffer(0)));
        queue.push_back(next_ptr);
        task.has_any_work = true;
      }
      if (seen.count(next_ptr) == 0) {
        seen.insert(next_ptr);
        search_queue.push_back(next_ptr);
      }
    }
  }
}

/** Computes the number of dependencies for each function which requires grad */
auto Engine::compute_dependencies(function_queue queue, GraphTask& task) -> void {
  // Just to make sure that they will never be added to the queue again
  std::unordered_set<Function*> seen(queue.begin(), queue.end());

  // Queue contains all nodes that will start propagating gradients.
  // We no longer have to expand functions that don't require grad.
  auto& dependencies = task.dependencies;
  while (queue.size() > 0) {
    auto fn = std::move(queue.back()); queue.pop_back();
    // This is needed only to filter out roots that aren't executable
    if (!fn->is_executable) continue;
    for (auto& next_fn_pair : fn->next_functions) {
      Function* next_ptr = next_fn_pair.first.get();
      if (!next_ptr) continue;
      if (!next_ptr->is_executable) continue;
      if (next_ptr->is_stochastic) continue; // Stochastic nodes were in the queue already
      dependencies[next_ptr] += 1;
      if (seen.count(next_ptr) == 0) {
        seen.insert(next_ptr);
        queue.push_back(next_ptr);
      }
    }
  }
}

auto Engine::find_roots(const function_list& input_roots,
                        variable_list& inputs,
                        GraphTask& task) -> function_queue {
  std::unordered_map<std::shared_ptr<Function>, std::unique_ptr<InputBuffer>> root_value;
  int num_inputs = input_roots.size();
  for (int i = 0; i < num_inputs; ++i) {
    auto& input = inputs[i];
    auto& root_info = input_roots[i];
    auto root = root_info.first;
    int input_nr = root_info.second;
    auto& buf = root_value[root];
    if (root->is_executable) {
      if (!buf) buf.reset(new InputBuffer(root->num_inputs));
      buf->add(input_nr, std::shared_ptr<Variable>(input));
    }
  }

  function_queue roots;
  for (auto& entry: root_value) {
    const auto& root = entry.first;
    roots.push_back(root.get());
    // no need to enqueue tasks for non-executable functions
    if (!root->is_executable) continue;
    auto& input_buf = entry.second;
    auto& queue = ready_queue(input_buf->device());
    queue.push_front(FunctionTask(&task, root, std::move(*input_buf)));
    task.has_any_work = true;
  }

  return roots;
}

auto Engine::execute(const function_list& input_roots,
                     variable_list& inputs,
                     bool keep_graph,
                     const callback_map& callbacks) -> void {
  std::call_once(start_threads_flag, &Engine::start_threads, this);

  GraphTask graph_task(keep_graph, callbacks);
  std::unique_lock<std::mutex> lock(graph_task.mutex);

  // Find the unique roots and backprop into variables.
  function_queue roots = find_roots(input_roots, inputs, graph_task);

  // Search the graph and find all stochastic functions. Append them to the queue.
  find_stochastic_functions(roots, graph_task);

  if (!graph_task.has_any_work) {
    throw std::runtime_error(
      "there are no graph nodes that require computing gradients");
  }

  // Now compute the dependencies for all executable functions
  compute_dependencies(std::move(roots), graph_task);

  // Wait for all tasks to complete
  graph_task.not_done.wait(lock, [&graph_task]{
    return graph_task.outstanding_tasks.load() == 0;
  });

  // Check for an exception while running backwards
  if (graph_task.has_error.load()) {
    std::rethrow_exception(graph_task.exception);
  }

  if (!graph_task.not_ready.empty()) {
    throw std::runtime_error("could not compute gradients for some functions");
  }
}

auto Engine::ready_queue(int device) -> ReadyQueue& {
  return *ready_queues.at(device + 1);
}

auto Engine::start_threads() -> void {
  int num_devices = 0;
#ifdef WITH_CUDA
  // check for case of compiled with CUDA but no available devices
  if (cudaGetDeviceCount(&num_devices) != cudaSuccess) {
    cudaGetLastError();
    num_devices = 0;
  }
#endif
  ready_queues = std::vector<std::shared_ptr<ReadyQueue>>(num_devices + 1);
  for (auto& queue : ready_queues) {
    queue.reset(new ReadyQueue());
    std::thread t(&Engine::thread_main, this, queue);
    t.detach();
  }
}

}} // namespace torch::autograd
