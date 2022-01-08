#pragma once

#include <memory>
#include <vector>
#include <THPP/THPP.h>

#include "torch/csrc/autograd/function.h"
#include "torch/csrc/autograd/variable.h"

#ifdef WITH_CUDNN
#include "torch/csrc/cudnn/Conv.h"
#else
namespace torch { namespace cudnn {
struct Convolution {};
}}
#endif

namespace torch { namespace autograd {

struct ConvParams {
  std::vector<int> stride;
  std::vector<int> padding;
  std::vector<int> dilation;
  bool transposed;
  std::vector<int> output_padding;
  int groups;
  bool benchmark;
  bool cudnn_enabled;

  bool is_dilated() const;
  bool is_output_padding_neg() const;
  bool is_padding_neg() const;
  void view1d_as_2d();
  bool use_cudnn(const thpp::Tensor& input) const;
};

struct ConvForward : public Function, public ConvParams {
  explicit ConvForward(ConvParams params) : ConvParams(std::move(params)) {}

  virtual variable_list apply(const variable_list& inputs) override;

  std::vector<long> output_size(thpp::Tensor& input, thpp::Tensor& weight);
};

struct ConvBackward : public Function, public ConvParams {
  ConvBackward(
      FunctionFlags flags,
      ConvParams params,
      SavedVariable input,
      SavedVariable weight,
      SavedVariable bias,
      tensor_list columns,
      tensor_list ones,
      std::unique_ptr<torch::cudnn::Convolution> convolution)
    : Function(std::move(flags))
    , ConvParams(std::move(params))
    , convolution(std::move(convolution)) {
      if (is_executable) {
        this->input_ = std::move(input);
        this->weight_ = std::move(weight);
        this->bias_ = std::move(bias);
        this->columns = std::move(columns);
        this->ones = std::move(ones);
      }
    }

  virtual variable_list apply(const variable_list& gradOutputs) override;

  virtual void releaseVariables() override;

  SavedVariable input_;
  SavedVariable weight_;
  SavedVariable bias_;
  tensor_list columns;
  tensor_list ones;
  std::unique_ptr<torch::cudnn::Convolution> convolution;
};

}}
