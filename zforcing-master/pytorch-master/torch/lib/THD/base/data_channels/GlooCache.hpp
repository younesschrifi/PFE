#pragma once

#include "../ChannelUtils.hpp"
#include "../DataChannel.hpp"

#include "gloo/algorithm.h"
#include "gloo/allgather_ring.h"
#include "gloo/allreduce_ring.h"
#include "gloo/barrier_all_to_all.h"
#include "gloo/broadcast_one_to_all.h"
#ifdef WITH_CUDA
#include "gloo/cuda_allreduce_ring.h"
#include "gloo/cuda_broadcast_one_to_all.h"
#endif
#include "gloo/rendezvous/context.h"
#include "gloo/rendezvous/store.h"
#include "gloo/rendezvous/prefix_store.h"

#ifdef WITH_CUDA
#include <cuda.h>
#include <THC/THC.h>
#endif
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#ifdef WITH_CUDA
// TODO: it should be possible to set this using the library API...
extern THCState *state;
#endif


namespace thd {
namespace gloo_cache {

using key_type = std::tuple<
  CollectiveType, // operation
  THDGroup,       // group
  DeviceType,     // tensors device type
  std::size_t,    // input buffer bytes
  std::size_t,    // output buffer bytes
  THDReduceOp,    // reduce op
  rank_type       // src/dest rank
>;

} // namespace gloo_cache
} // namespace thd


MAKE_HASHABLE(
  thd::gloo_cache::key_type,
  std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t),
  std::get<4>(t), std::get<5>(t), std::get<6>(t)
);


namespace thd {

// Forward declaration
template<CollectiveType D, typename T>
struct algorithm_spec;

struct GlooCache {
  using buffer_type = char;
  using algorithm_type = ::gloo::Algorithm;
  using context_type = ::gloo::rendezvous::Context;
  using prefix_store_type = ::gloo::rendezvous::PrefixStore;
  using store_type = ::gloo::rendezvous::Store;

  using key_type = gloo_cache::key_type;
  using value_type = std::tuple<
    std::shared_ptr<algorithm_type>, // algorithm
    std::shared_ptr<buffer_type>,    // input buffer (nullptr if not used)
    std::shared_ptr<buffer_type>,    // output buffer (nullptr if not used)
    std::shared_ptr<std::mutex>      // mutex to protect same algorithm from running concurrently
  >;

  GlooCache(rank_type rank, std::shared_ptr<::gloo::transport::Device> device,
            std::shared_ptr<store_type> store)
   : _rank(rank)
   , _device(device)
   , _store(store)
  {}

  GlooCache(GlooCache const&)      = delete;
  void operator=(GlooCache const&) = delete;


  // Accessors for value_type tuple
  static inline std::shared_ptr<algorithm_type> algorithm(const value_type& t) {
    return std::get<0>(t);
  }

  static inline std::shared_ptr<buffer_type> input_buffer(const value_type& t) {
    return std::get<1>(t);
  }

  static inline std::shared_ptr<buffer_type> output_buffer(const value_type& t) {
    return std::get<2>(t);
  }

  static inline std::shared_ptr<std::mutex> mutex(const value_type& t) {
    return std::get<3>(t);
  }


  std::shared_ptr<context_type> createContext(
    const DataChannel::Group& group,
    prefix_store_type& store
  ) {
    auto context = std::make_shared<context_type>(group.mustGetGroupRank(_rank), group.size());
    context->connectFullMesh(store, _device);
    return context;
  }

  std::shared_ptr<buffer_type> createBuffer(std::size_t bytes, DeviceType device) const {
    if (device == DeviceType::CPU) {
      return std::shared_ptr<buffer_type>(new char[bytes],
                                          std::default_delete<char[]>());
#ifdef WITH_CUDA
    } else if (device == DeviceType::CUDA) {
      buffer_type *buf;
      THCudaCheck(THCudaMalloc(state, (void**)&buf, bytes));
      return std::shared_ptr<buffer_type>(buf, [](char* ptr) { THCudaFree(state, ptr); });
#endif
    } else {
      throw std::runtime_error("unsupported device in GlooCache::createBuffer");
    }
  }

  template<CollectiveType D, typename T, typename... Args>
  value_type getAlgorithm(THDGroup group_id, const DataChannel::Group& group,
                          Args... args) {
    // We need to protect from race when two (or more) threads are trying to
    // create same algorithm simultaneously.
    std::lock_guard<std::mutex> lock(_mutex);

    auto key = algorithm_spec<D, T>::key(group_id, args...);
    auto it = _algorithms.find(key);
    if (it == _algorithms.end()) {
      // create prefix store with unique prefix
      prefix_store_type prefix_store(print_key(key), *_store);
      std::tie(it, std::ignore) = _algorithms.emplace(std::make_pair(
        key,
        algorithm_spec<D, T>::create(*this, group, prefix_store, std::forward<Args>(args)...)
      ));
    }

    return it->second;
  }

  static void memcpy_input(value_type& info, thpp::Tensor& t) {
    std::uint64_t tensor_bytes = t.elementSize() * t.numel();
    auto t_dev = getDeviceType(t);
    auto input_buffer = GlooCache::input_buffer(info).get();

    if (t_dev == DeviceType::CPU) {
      std::memcpy(input_buffer, t.data(), tensor_bytes);
#ifdef WITH_CUDA
    } else if (t_dev == DeviceType::CUDA) {
      THCudaCheck(cudaMemcpy(input_buffer, t.data(), tensor_bytes, cudaMemcpyDeviceToDevice));
      THCudaCheck(cudaDeviceSynchronize()); // TODO: we probably want to have a more fine-grained sync
#endif
    } else {
      throw std::runtime_error("unsupported device in memcpy_input");
    }
  }

  static void memcpy_output(value_type& info, thpp::Tensor& t) {
    std::uint64_t tensor_bytes = t.elementSize() * t.numel();
    auto t_dev = getDeviceType(t);
    auto output_buffer = GlooCache::output_buffer(info).get();

    if (t_dev == DeviceType::CPU) {
      std::memcpy(t.data(), output_buffer, tensor_bytes);
#ifdef WITH_CUDA
    } else if (t_dev == DeviceType::CUDA) {
      THCudaCheck(cudaMemcpy(t.data(), output_buffer, tensor_bytes, cudaMemcpyDeviceToDevice));
#endif
    } else {
      throw std::runtime_error("unsupported device in memcpy_input");
    }
  }



private:
  std::string print_key(const key_type& k) {
    return std::to_string(static_cast<uint8_t>(std::get<0>(k))) + "-"
      + std::to_string(std::get<1>(k)) + "-"
      + std::to_string(static_cast<uint8_t>(std::get<2>(k))) + "-"
      + std::to_string(std::get<3>(k)) + "-"
      + std::to_string(std::get<4>(k)) + "-"
      + std::to_string(std::get<5>(k)) + "-"
      + std::to_string(std::get<6>(k));
  }

  rank_type _rank;
  std::shared_ptr<::gloo::transport::Device> _device;
  std::shared_ptr<store_type> _store;

  std::mutex _mutex;

  std::unordered_map<key_type, value_type> _algorithms;
};

template<typename T>
const ::gloo::ReductionFunction<T>* THDToGlooReduceOp(THDReduceOp op) {
  switch (op) {
    case THDReduceMIN:
      return ::gloo::ReductionFunction<T>::min;
    case THDReduceMAX:
      return ::gloo::ReductionFunction<T>::max;
    case THDReduceSUM:
      return ::gloo::ReductionFunction<T>::sum;
    case THDReducePRODUCT:
      return ::gloo::ReductionFunction<T>::product;
    default:
      throw std::invalid_argument("unknown reduce operation");
  }
}

const DeviceType UNUSED_DEVICE = DeviceType::LAST;
const THDReduceOp UNUSED_OP = THDReduceMIN;
const rank_type UNUSED_RANK = -1;
const std::size_t UNUSED_BYTES = 0;

template<typename T>
struct algorithm_spec<CollectiveType::ALL_GATHER, T> {
  static GlooCache::key_type key(
    THDGroup group_id, DeviceType device, std::size_t input_bytes, std::size_t output_bytes, std::size_t unused_count
  ) {
    return std::make_tuple(CollectiveType::ALL_GATHER, group_id, device,
                           input_bytes, output_bytes, UNUSED_OP, UNUSED_RANK);
  }

  static GlooCache::value_type create(GlooCache& cache,
    const DataChannel::Group& group, GlooCache::prefix_store_type& store,
    DeviceType device, std::size_t input_bytes, std::size_t output_bytes, std::size_t count
  ) {
    auto context = cache.createContext(group, store);
    auto input_buffer = cache.createBuffer(input_bytes, device);
    auto output_buffer = cache.createBuffer(output_bytes, device);

    std::shared_ptr<GlooCache::algorithm_type> algo;
    if (device == DeviceType::CPU) {
      algo = std::make_shared<::gloo::AllgatherRing<T>>(
        context,
        std::initializer_list<T*>{reinterpret_cast<T*>(input_buffer.get())},
        reinterpret_cast<T*>(output_buffer.get()),
        count);
    } else {
      throw std::runtime_error("unsupported device in Gloo allGather");
    }

    return std::make_tuple(
      algo,
      input_buffer,
      output_buffer,
      std::make_shared<std::mutex>()
    );
  }
};

template<typename T>
struct algorithm_spec<CollectiveType::ALL_REDUCE, T> {
  static GlooCache::key_type key(
    THDGroup group_id, DeviceType device, std::size_t input_bytes,
    std::size_t unused_count, THDReduceOp op
  ) {
    return std::make_tuple(CollectiveType::ALL_REDUCE, group_id, device,
                           input_bytes, input_bytes, op, UNUSED_RANK);
  }

  static GlooCache::value_type create(GlooCache& cache,
    const DataChannel::Group& group, GlooCache::prefix_store_type& store,
    DeviceType device, std::size_t input_bytes, std::size_t count, THDReduceOp op
  ) {
    auto context = cache.createContext(group, store);
    auto input_buffer = cache.createBuffer(input_bytes, device);

    std::shared_ptr<GlooCache::algorithm_type> algo;
    if (device == DeviceType::CPU) {
      algo = std::make_shared<::gloo::AllreduceRing<T>>(
        context,
        std::initializer_list<T*>{reinterpret_cast<T*>(input_buffer.get())},
        count,
        THDToGlooReduceOp<T>(op));
#ifdef WITH_CUDA
    } else if (device == DeviceType::CUDA) {
      if (op != THDReduceSUM) {
        throw std::runtime_error("Gloo backend only supports sum op for CUDA all reduce");
      }
      algo = std::make_shared<::gloo::CudaAllreduceRing<T>>(
        context,
        std::initializer_list<T*>{reinterpret_cast<T*>(input_buffer.get())},
        count); // TODO: figure out what to do with streams
#endif
    } else {
      throw std::runtime_error("unsupported tensor device in Gloo allReduce");
    }

    return std::make_tuple(
      algo,
      input_buffer,
      input_buffer, // we get the result in same buffer
      std::make_shared<std::mutex>()
    );
  }
};

template<typename T>
struct algorithm_spec<CollectiveType::BROADCAST, T> {
  static GlooCache::key_type key(
    THDGroup group_id, DeviceType device, std::size_t input_bytes,
    std::size_t unused_count, rank_type src_rank
  ) {
    return std::make_tuple(CollectiveType::BROADCAST, group_id, device,
                           input_bytes, input_bytes, UNUSED_OP, src_rank);
  }

  static GlooCache::value_type create(GlooCache& cache,
    const DataChannel::Group& group, GlooCache::prefix_store_type& store,
    DeviceType device, std::size_t input_bytes, std::size_t count, rank_type src_rank
  ) {
    auto context = cache.createContext(group, store);
    auto input_buffer = cache.createBuffer(input_bytes, device);

    std::shared_ptr<GlooCache::algorithm_type> algo;
    if (device == DeviceType::CPU) {
      algo = std::make_shared<::gloo::BroadcastOneToAll<T>>(
        context,
        std::initializer_list<T*>{reinterpret_cast<T*>(input_buffer.get())},
        count,
        src_rank);
#ifdef WITH_CUDA
    } else if (device == DeviceType::CUDA) {
      algo = std::make_shared<::gloo::CudaBroadcastOneToAll<T>>(
        context,
        std::initializer_list<T*>{reinterpret_cast<T*>(input_buffer.get())},
        count,
        src_rank); // TODO: figure out what to do with streams
#endif
    } else {
      throw std::runtime_error("unsupported tensor device in Gloo broadcast");
    }

    return std::make_tuple(
      algo,
      input_buffer,
      input_buffer, // we get the result in same buffer
      std::make_shared<std::mutex>()
    );
  }
};

template<typename T> // unused
struct algorithm_spec<CollectiveType::BARRIER, T> {
  static GlooCache::key_type key(THDGroup group_id) {
    return std::make_tuple(CollectiveType::BARRIER, group_id, UNUSED_DEVICE,
                           UNUSED_BYTES, UNUSED_BYTES, UNUSED_OP, UNUSED_RANK);
  }

  static GlooCache::value_type create(GlooCache& cache,
    const DataChannel::Group& group, GlooCache::prefix_store_type& store
  ) {
    auto context = cache.createContext(group, store);
    return std::make_tuple(
      std::make_shared<::gloo::BarrierAllToAll>(context),
      nullptr,
      nullptr,
      std::make_shared<std::mutex>()
    );
  }
};

} // namespace thd
