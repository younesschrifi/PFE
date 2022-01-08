#include <Python.h>
#include <structmember.h>

#include <TH/THMath.h>
#include <stdbool.h>
#include <vector>
#include <stack>
#include <tuple>
#include "torch/csrc/cuda/THCP.h"

#include "torch/csrc/cuda/override_macros.h"
#include "torch/csrc/copy_utils.h"
#include "DynamicTypes.h"

//generic_include THC torch/csrc/generic/Tensor.cpp

#include "torch/csrc/cuda/undef_macros.h"
#include "torch/csrc/cuda/restore_macros.h"
