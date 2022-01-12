#include <Python.h>
#include <structmember.h>

#define THP_HOST_HALF

#include <stdbool.h>
#include <vector>
#include <stack>
#include <tuple>
#include <TH/THMath.h>

#include "torch/csrc/THP.h"
#include "torch/csrc/copy_utils.h"
#include "torch/csrc/DynamicTypes.h"

#define TH_GENERIC_FILE "torch/csrc/generic/Tensor.cpp"
#include "TH/THGenerateByteType.h"
