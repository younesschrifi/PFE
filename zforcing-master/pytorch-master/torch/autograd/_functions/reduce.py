from functools import reduce

from ..function import Function
from ..variable import Variable


class Sum(Function):

    @staticmethod
    def forward(ctx, input, dim=None):
        ctx.dim = dim
        ctx.input_size = input.size()
        if dim is None:
            return input.new((input.sum(),))
        else:
            return input.sum(dim)

    @staticmethod
    def backward(ctx, grad_output):
        if ctx.dim is None:
            return grad_output.expand(ctx.input_size), None
        else:
            repeats = [1 for _ in ctx.input_size]
            repeats[ctx.dim] = ctx.input_size[ctx.dim]
            return grad_output.repeat(*repeats), None


class Prod(Function):

    @staticmethod
    def forward(ctx, input, dim=None):
        ctx.dim = dim
        ctx.input_size = input.size()
        if dim is None:
            ctx.result = input.prod()
            ctx.save_for_backward(input)
            return input.new((ctx.result,))
        else:
            output = input.prod(dim)
            ctx.save_for_backward(input, output)
            return output

    @staticmethod
    def backward(ctx, grad_output):
        if ctx.dim is None:
            input, = ctx.saved_variables
            zero_idx = (input.data == 0).nonzero()
            if zero_idx.dim() == 0:
                return grad_output.mul(ctx.result).expand_as(input).div(input), None
            elif zero_idx.size(0) > 1:
                return Variable(grad_output.data.new(ctx.input_size).zero_()), None
            else:
                grad_input = Variable(grad_output.data.new(ctx.input_size).zero_())
                zero_idx = tuple(zero_idx[0].cpu())
                to_add = input.data.new(ctx.input_size).zero_()
                to_add[zero_idx] = 1.
                grad_input[zero_idx] = grad_output * (input + Variable(to_add)).prod()
                return grad_input, None
        else:
            input, output = ctx.saved_variables
            dim = ctx.dim if ctx.dim >= 0 else ctx.dim + input.dim()
            zero_mask = input == 0
            slice_zero_count = zero_mask.sum(dim)
            total_zeros = slice_zero_count.sum()
            grad_input = grad_output.mul(output).expand_as(input).div(input)
            if total_zeros == 0:
                return grad_input, None

            some_zeros = slice_zero_count.gt(0).expand_as(grad_input)
            grad_input[some_zeros] = 0

            single_zero_idx = slice_zero_count.eq(1).nonzero()

            if len(single_zero_idx) == 0:
                return grad_input, None

            for idx in single_zero_idx:
                idx_tuple = tuple(idx.cpu())
                input_idx_tuple = idx_tuple[:dim] + (slice(0, None),) + idx_tuple[dim + 1:]

                # slice_mask and input_copy are 1D
                slice_mask = zero_mask[input_idx_tuple]
                input_copy = input[input_idx_tuple].clone()
                zero_idx = slice_mask.nonzero()[0, 0]
                input_copy[zero_idx] = 1.

                grad_idx_tuple = idx_tuple[:dim] + (zero_idx,) + idx_tuple[dim + 1:]
                grad_input[grad_idx_tuple] = grad_output[idx_tuple] * input_copy.prod()

            return grad_input, None


class Mean(Function):

    @staticmethod
    def forward(ctx, input, dim=None):
        ctx.dim = dim
        ctx.input_size = input.size()
        if dim is None:
            return input.new((input.mean(),))
        else:
            return input.mean(dim)

    @staticmethod
    def backward(ctx, grad_output):
        if ctx.dim is None:
            grad_input_val = grad_output / reduce(lambda x, y: x * y, ctx.input_size, 1)
            return grad_input_val.expand(ctx.input_size), None
        else:
            repeats = [1 for _ in ctx.input_size]
            dim_size = ctx.input_size[ctx.dim]
            repeats[ctx.dim] = dim_size
            return grad_output.repeat(*repeats).div_(dim_size), None


class _SelectionFunction(Function):
    has_all_reduce = True
    # additional_args is prepended before dim when calling the tensor
    # function. It's a no-op for subclasses other than kthvalue.
    # kthvalue not only requires us to pass a dim, but also preceed it with k.
    additional_args = tuple()

    def __init__(self, dim=None):
        super(_SelectionFunction, self).__init__()
        self.dim = dim

    def forward(self, input):
        fn = getattr(input, type(self).__name__.lower())
        self.input_size = input.size()
        if self.dim is None and self.has_all_reduce:
            value = fn(*self.additional_args)
            self.indices = tuple(input.eq(value).nonzero()[0])
            return input.new((value,))
        else:
            if self.dim is None:
                dim = input.dim() - 1
            else:
                dim = self.dim
            args = (dim,)
            if self.additional_args:
                args = self.additional_args + args
            output, indices = fn(*args)
            self.save_for_backward(indices)
            self.mark_non_differentiable(indices)
            return output, indices

    def backward(self, grad_output, grad_indices=None):
        grad_input = grad_output.new(*self.input_size).zero_()
        if self.dim is None and self.has_all_reduce:
            grad_input[self.indices] = grad_output[0]
        else:
            if self.dim is None:
                dim = input.dim() - 1
            else:
                dim = self.dim
            indices, = self.saved_tensors
            grad_input.scatter_(dim, indices, grad_output)
        return grad_input


class Max(_SelectionFunction):
    pass


class Min(_SelectionFunction):
    pass


class Mode(_SelectionFunction):
    has_all_reduce = False


class Median(_SelectionFunction):
    has_all_reduce = False


class Kthvalue(_SelectionFunction):
    has_all_reduce = False

    def __init__(self, k, dim=None):
        super(Kthvalue, self).__init__(dim)
        self.additional_args = (k,)


class Norm(Function):

    def __init__(self, norm_type=2, dim=None):
        super(Norm, self).__init__()
        self.norm_type = norm_type
        self.dim = dim

    def forward(self, input):
        if self.dim is None:
            self.norm = input.norm(self.norm_type)
            self.save_for_backward(input)
            return input.new((self.norm,))
        else:
            output = input.norm(self.norm_type, self.dim)
            self.save_for_backward(input, output)
            return output

    def backward(self, grad_output):
        if self.dim is None:
            input, = self.saved_tensors
            if self.norm_type == 2:
                return input.mul(grad_output[0] / self.norm)
            else:
                pow = input.abs().pow(self.norm_type - 2)
                scale = grad_output[0] / self.norm ** (self.norm_type - 1)
                return input.mul(pow).mul(scale)
        else:
            input, output = self.saved_tensors
            big_grad_output = grad_output.expand_as(input)
            if self.norm_type == 2:
                big_output = output.expand_as(input)
                return input.mul(big_grad_output).div(big_output)
            else:
                pow = input.abs().pow(self.norm_type - 2)
                big_output = output.pow(self.norm_type - 1).expand_as(input)
                return input.mul(pow).mul(big_grad_output).div(big_output)


# TODO: renorm
# TODO: std
# TODO: var
