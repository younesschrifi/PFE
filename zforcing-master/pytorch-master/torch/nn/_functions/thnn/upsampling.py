from numbers import Integral
import torch
from torch.autograd import Function
from torch._thnn import type2backend

from . import _all_functions
from ...modules.utils import _pair
from ...functional import _check_bilinear_2d_scale_factor


class _UpsamplingBase(Function):

    def __init__(self, size=None, scale_factor=None):
        super(_UpsamplingBase, self).__init__()
        if size is None and scale_factor is None:
            raise ValueError('either size or scale_factor should be defined')
        if scale_factor is not None and not isinstance(scale_factor, (Integral, tuple)):
            raise ValueError('scale_factor must be of integer type or tuple of integer types')
        self.size = size
        self.scale_factor = scale_factor


class UpsamplingNearest2d(_UpsamplingBase):

    def __init__(self, size=None, scale_factor=None):
        super(UpsamplingNearest2d, self).__init__(size, scale_factor)

        if self.scale_factor is not None and not isinstance(scale_factor, Integral):
            raise ValueError('scale_factor must be of integer type for nearest neighbor sampling')

        self.size = _pair(self.size) if self.size is not None else None

    def forward(self, input):
        assert input.dim() == 4

        if self.scale_factor is None:
            if (self.size[0] % input.size(2) != 0 or
                    self.size[1] % input.size(3) != 0):
                raise RuntimeError("output size specified in UpSamplingNearest "
                                   "({}) has to be divisible by the input size, but got: "
                                   "{}".format('x'.join(map(str, self.size)),
                                               'x'.join(map(str, input.size()))))
            self.scale_factor = self.size[0] // input.size(2)
            if self.scale_factor != self.size[1] // input.size(3):
                raise RuntimeError("input aspect ratio doesn't match the "
                                   "output ratio")

        output = input.new()
        backend = type2backend[type(input)]
        self.save_for_backward(input)
        backend.SpatialUpSamplingNearest_updateOutput(
            backend.library_state,
            input,
            output,
            self.scale_factor
        )
        return output

    def backward(self, grad_output):
        input, = self.saved_tensors
        grad_input = grad_output.new()
        backend = type2backend[type(input)]
        backend.SpatialUpSamplingNearest_updateGradInput(
            backend.library_state,
            input,
            grad_output,
            grad_input,
            self.scale_factor
        )
        return grad_input


class UpsamplingBilinear2d(_UpsamplingBase):

    def __init__(self, size=None, scale_factor=None):
        super(UpsamplingBilinear2d, self).__init__(size, scale_factor)

        if self.scale_factor is not None:
            self.scale_factor = _check_bilinear_2d_scale_factor(self.scale_factor)

        self.size = _pair(self.size) if self.size is not None else None

    def forward(self, input):
        assert input.dim() == 4

        if self.scale_factor is not None:
            self.output_size = (
                input.size(2) * self.scale_factor[0],
                input.size(3) * self.scale_factor[1],
            )
        else:
            self.output_size = self.size

        self.input_size = input.size()
        output = input.new()
        backend = type2backend[type(input)]
        backend.SpatialUpSamplingBilinear_updateOutput(
            backend.library_state,
            input,
            output,
            self.output_size[0],
            self.output_size[1],
        )
        return output

    def backward(self, grad_output):
        assert grad_output.dim() == 4

        grad_output = grad_output.contiguous()
        grad_input = grad_output.new()
        backend = type2backend[type(grad_output)]
        backend.SpatialUpSamplingBilinear_updateGradInput(
            backend.library_state,
            grad_output,
            grad_input,
            self.input_size[0],
            self.input_size[1],
            self.input_size[2],
            self.input_size[3],
            self.output_size[0],
            self.output_size[1],
        )
        return grad_input

    def __setstate__(self, state):
        self.__dict__.update(state)
        self.scale_factor = _tuple(self.scale_factor)

_all_functions.append(UpsamplingNearest2d)
_all_functions.append(UpsamplingBilinear2d)
