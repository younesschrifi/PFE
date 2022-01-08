from functools import reduce
import torch
from torch._utils import _accumulate

from ..function import Function, InplaceFunction, once_differentiable
from ..variable import Variable


class Index(Function):

    @staticmethod
    def forward(ctx, i, index):
        ctx.input_size = i.size()
        ctx.index = index
        result = i.index(ctx.index)
        ctx.mark_shared_storage((i, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        grad_input = Variable(grad_output.data.new(ctx.input_size).zero_())
        grad_input[ctx.index] = grad_output
        return grad_input, None


class SetItem(InplaceFunction):

    @staticmethod
    def forward(ctx, i, index, value):
        assert not isinstance(index, Variable)
        ctx.mark_dirty(i)
        ctx.index = index
        ctx.tensor_value = torch.is_tensor(value)
        if ctx.tensor_value:
            ctx.value_size = value.size()
        i._set_index(ctx.index, value)
        return i

    @staticmethod
    def backward(ctx, grad_output):
        grad_input = grad_output.clone()
        grad_input[ctx.index] = 0
        grad_value = None
        if ctx.tensor_value:
            grad_value = grad_output[ctx.index].contiguous().view(ctx.value_size)
        return grad_input, None, grad_value


# TODO: how to do NoGrad in new style
class NoGrad(Function):

    def forward(self, i):
        result = i.new(i)
        self.mark_non_differentiable(result)
        self.mark_shared_storage((i, result))
        return result

    def backward(self, grad_output):
        assert False, "backward of NoGrad should never be called"

    def _do_forward(self, *args, **kwargs):
        result = super(NoGrad, self)._do_forward(*args, **kwargs)
        self.requires_grad = False
        return result

    __call__ = _do_forward


class Transpose(Function):

    @staticmethod
    def forward(ctx, i, dim1, dim2):
        result = i.transpose(dim1, dim2)
        ctx.dims = (dim1, dim2)
        ctx.mark_shared_storage((i, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.transpose(*ctx.dims), None, None


class View(Function):

    @staticmethod
    def forward(ctx, i, sizes):
        ctx.new_sizes = sizes
        ctx.old_size = i.size()
        result = i.view(*sizes)
        ctx.mark_shared_storage((i, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.contiguous().view(ctx.old_size), None


class Expand(Function):

    @staticmethod
    def forward(ctx, i, new_size):
        ctx.num_unsqueezed = len(new_size) - i.dim()
        ctx.expanded_dims = [dim for dim, (expanded, original)
                             in enumerate(zip(new_size[ctx.num_unsqueezed:], i.size()))
                             if expanded != original]
        result = i.expand(*new_size)
        ctx.mark_shared_storage((i, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        grad_input = grad_output
        for i in range(ctx.num_unsqueezed):
            grad_input = grad_input.sum(0).squeeze(0)
        for dim in ctx.expanded_dims:
            grad_input = grad_input.sum(dim)
        return grad_input, None


class Type(Function):

    @staticmethod
    def forward(ctx, i, dest_type):
        ctx.input_type = type(i)
        return i.type(dest_type)

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.type(ctx.input_type), None


class CudaTransfer(Function):

    @staticmethod
    def forward(ctx, i, device_id=None, async=False):
        ctx.source_device = -1 if not i.is_cuda else i.get_device()
        ctx.source_was_cuda = i.is_cuda
        if device_id:
            return i.cuda(device_id, async=async)
        else:
            return i.cuda(async=async)

    @staticmethod
    def backward(ctx, grad_output):
        if ctx.source_device != -1:
            return grad_output.cuda(ctx.source_device), None, None
        elif ctx.source_was_cuda:
            return grad_output, None, None
        else:
            return grad_output.cpu(), None, None


class Permute(Function):

    @staticmethod
    def forward(ctx, input, dim_indices):
        ctx.rev_dim_indices = [None for _ in range(len(dim_indices))]
        for i, dim_idx in enumerate(dim_indices):
            ctx.rev_dim_indices[dim_idx] = i
        result = input.permute(*dim_indices)
        ctx.mark_shared_storage((input, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.permute(*ctx.rev_dim_indices), None


class IndexAdd(InplaceFunction):

    @staticmethod
    def forward(ctx, tensor1, dim, index, tensor2, inplace=False):
        assert not ctx.needs_input_grad[2]
        ctx.dim = dim
        if ctx.needs_input_grad[3]:
            ctx.save_for_backward(index)
        if not inplace:
            tensor1 = tensor1.clone()
        else:
            ctx.mark_dirty(tensor1)
        return tensor1.index_add_(ctx.dim, index, tensor2)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        grad_tensor1 = grad_tensor2 = None

        if ctx.needs_input_grad[0]:
            grad_tensor1 = grad_output

        if ctx.needs_input_grad[3]:
            index, = ctx.saved_tensors
            grad_tensor2 = grad_output.index_select(ctx.dim, index)

        return grad_tensor1, None, None, grad_tensor2, None


class IndexCopy(InplaceFunction):

    @staticmethod
    def forward(ctx, tensor1, dim, index, tensor2, inplace=False):
        assert not ctx.needs_input_grad[2]
        ctx.dim = dim
        if any(ctx.needs_input_grad):
            ctx.save_for_backward(index)
        if not inplace:
            tensor1 = tensor1.clone()
        else:
            ctx.mark_dirty(tensor1)
        return tensor1.index_copy_(dim, index, tensor2)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        grad_tensor1 = grad_tensor2 = None

        if any(ctx.needs_input_grad):
            index, = ctx.saved_tensors

        if ctx.needs_input_grad[0]:
            grad_tensor1 = grad_output.clone().index_fill_(ctx.dim, index, 0)

        if ctx.needs_input_grad[2]:
            grad_tensor2 = grad_output.index_select(ctx.dim, index)

        return grad_tensor1, None, None, grad_tensor2, None


class IndexFill(InplaceFunction):

    @staticmethod
    def forward(ctx, tensor, dim, index, value, inplace=False):
        ctx.dim = dim
        assert not ctx.needs_input_grad[2]
        if ctx.needs_input_grad[0]:
            ctx.save_for_backward(index)
        if not inplace:
            tensor = tensor.clone()
        else:
            ctx.mark_dirty(tensor)
        return tensor.index_fill_(dim, index, value)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        grad_tensor = None

        if ctx.needs_input_grad[0]:
            index, = ctx.saved_tensors
            grad_tensor = grad_output.clone().index_fill_(ctx.dim, index, 0)

        return grad_tensor, None, None, None, None


class IndexSelect(Function):

    @staticmethod
    def forward(ctx, tensor, dim, index):
        ctx.dim = dim
        assert not ctx.needs_input_grad[2]

        if ctx.needs_input_grad[0]:
            ctx.save_for_backward(index)
            ctx.input_size = tensor.size()

        return tensor.index_select(dim, index)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        grad_tensor = None

        if ctx.needs_input_grad[0]:
            index, = ctx.saved_tensors
            grad_tensor = grad_output.new(*ctx.input_size).zero_()
            grad_tensor.index_add_(ctx.dim, index, grad_output)

        return grad_tensor, None, None


class Concat(Function):

    @staticmethod
    def forward(ctx, dim, *inputs):
        ctx.dim = dim
        ctx.input_sizes = [i.size(dim) for i in inputs]
        return torch.cat(inputs, dim)

    @staticmethod
    def backward(ctx, grad_output):
        return (None,) + tuple(grad_output.narrow(ctx.dim, end - size, size) for size, end
                               in zip(ctx.input_sizes, _accumulate(ctx.input_sizes)))


# TODO: deprecate this
class Resize(Function):

    @staticmethod
    def forward(ctx, tensor, sizes):
        ctx.sizes = sizes
        ctx.numel = reduce(lambda x, y: x * y, sizes, 1)
        if tensor.numel() != ctx.numel:
            raise RuntimeError(("requested resize to {} ({} elements in total), "
                                "but the given tensor has a size of {} ({} elements). "
                                "autograd's resize can only change the shape of a given "
                                "tensor, while preserving the number of elements. ").format(
                'x'.join(map(str, sizes)), ctx.numel,
                'x'.join(map(str, tensor.size())), tensor.numel()))
        ctx.input_sizes = tensor.size()
        if tensor.is_contiguous():
            result = tensor.new(tensor).contiguous().view(*sizes)
            ctx.mark_shared_storage((tensor, result))
            return result
        else:
            return tensor.contiguous().view(*sizes)

    @staticmethod
    def backward(ctx, grad_output):
        assert grad_output.numel() == ctx.numel
        return grad_output.contiguous().view(ctx.input_sizes), None


class Clone(Function):

    @staticmethod
    def forward(ctx, input):
        return input.clone()

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output


class Squeeze(Function):

    @staticmethod
    def forward(ctx, input, dim=None):
        ctx.dim = dim
        ctx.input_size = input.size()
        if dim is not None:
            result = input.squeeze(dim)
        else:
            result = input.squeeze()
        ctx.mark_shared_storage((input, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.contiguous().view(ctx.input_size), None


class Unsqueeze(Function):

    @staticmethod
    def forward(ctx, input, dim):
        ctx.dim = dim
        result = input.unsqueeze(dim)
        ctx.mark_shared_storage((input, result))
        return result

    @staticmethod
    def backward(ctx, grad_output):
        return grad_output.squeeze(ctx.dim), None


class MaskedCopy(InplaceFunction):

    @staticmethod
    def forward(ctx, tensor1, mask, tensor2, inplace=False):
        assert not ctx.needs_input_grad[1], "MaskedCopy can't differentiate the mask"
        if not inplace:
            tensor1 = tensor1.clone()
        else:
            ctx.mark_dirty(tensor1)
        ctx.save_for_backward(mask)
        return tensor1.masked_copy_(mask, tensor2)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        mask, = ctx.saved_tensors
        grad_tensor1 = grad_tensor2 = None
        if ctx.needs_input_grad[0]:
            grad_tensor1 = grad_output.clone().masked_fill_(mask, 0)
        if ctx.needs_input_grad[2]:
            grad_tensor2 = grad_output.masked_select(mask)
        return grad_tensor1, None, grad_tensor2, None


class MaskedFill(InplaceFunction):

    @staticmethod
    def forward(ctx, tensor, mask, value, inplace=False):
        assert not ctx.needs_input_grad[1], "MaskedFill can't differentiate the mask"
        if not inplace:
            tensor = tensor.clone()
        else:
            ctx.mark_dirty(tensor)
        ctx.save_for_backward(mask)
        return tensor.masked_fill_(mask, value)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        mask, = ctx.saved_tensors
        grad_tensor = None
        if ctx.needs_input_grad[0]:
            grad_tensor = grad_output.clone().masked_fill_(mask, 0)
        return grad_tensor, None, None, None


class MaskedSelect(Function):

    @staticmethod
    def forward(ctx, tensor, mask):
        assert not ctx.needs_input_grad[1], "MaskedSelect can't differentiate the mask"
        ctx.input_size = tensor.size()
        ctx.save_for_backward(mask)
        return tensor.masked_select(mask)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        mask, = ctx.saved_tensors
        grad_tensor = None
        if ctx.needs_input_grad[0]:
            grad_tensor = grad_output.new(ctx.input_size).zero_()
            grad_tensor.masked_copy_(mask, grad_output)
        return grad_tensor, None


class _MultiSelectionFunction(Function):

    @staticmethod
    def forward(ctx, input, dim, return_indices, args):
        fn = getattr(input, ctx._forward_cls.__name__.lower())
        ctx.return_indices = return_indices
        ctx.input_size = input.size()
        ctx.dim = dim
        output, indices = fn(*args)
        if return_indices:
            ctx.save_for_backward(indices)
            ctx.mark_non_differentiable(indices)
            return output, indices
        else:
            ctx.indices = indices
            return output

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output, grad_indices=None):
        grad_input = grad_output.new(ctx.input_size).zero_()
        if ctx.return_indices:
            indices, = ctx.saved_tensors
        else:
            indices = ctx.indices
        dim = ctx.dim if ctx.dim is not None else grad_output.dim() - 1
        return (grad_input.scatter_(dim, indices, grad_output),) + (None,) * ctx.num_flags


class Sort(_MultiSelectionFunction):

    @staticmethod
    def forward(ctx, input, dim=None, descending=False, return_indices=True):
        ctx.dim = dim if dim is not None else input.dim() - 1
        args = (ctx.dim, descending)
        ctx.num_flags = 3
        return _MultiSelectionFunction.forward(ctx, input, dim, return_indices, args)


class Topk(_MultiSelectionFunction):

    @staticmethod
    def forward(ctx, input, k, dim=None, largest=True, sort=True, return_indices=True):
        ctx.dim = dim if dim is not None else input.dim() - 1
        args = (k, ctx.dim, largest, sort)
        ctx.num_flags = 5
        return _MultiSelectionFunction.forward(ctx, input, dim, return_indices, args)


class Chunk(Function):

    @staticmethod
    def forward(ctx, i, num_chunks, dim=0):
        ctx.dim = dim
        result = i.chunk(num_chunks, dim)
        ctx.mark_shared_storage(*((i, chunk) for chunk in result))
        return result

    @staticmethod
    def backward(ctx, *grad_output):
        grad_input = torch.cat(grad_output, ctx.dim)
        return grad_input, None, None


class Gather(Function):

    @staticmethod
    def forward(ctx, input, dim, index):
        assert not ctx.needs_input_grad[2], "Gather can't differentiate the index"
        ctx.input_size = input.size()
        ctx.save_for_backward(index)
        ctx.dim = dim
        return input.gather(dim, index)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        index, = ctx.saved_tensors
        grad_input = grad_output.new(ctx.input_size).zero_()
        return grad_input.scatter_(ctx.dim, index, grad_output), None, None


class Scatter(InplaceFunction):

    @staticmethod
    def forward(ctx, input, dim, index, source, inplace=False):
        assert not ctx.needs_input_grad[2], "Scatter can't differentiate the index"
        ctx.dim = dim
        if inplace:
            ctx.mark_dirty(input)
        else:
            input = input.clone()
        ctx.save_for_backward(index)
        return input.scatter_(ctx.dim, index, source)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        index, = ctx.saved_tensors
        grad_input = grad_source = None
        if ctx.needs_input_grad[0]:
            grad_input = grad_output.clone()
            grad_input.scatter_(ctx.dim, index, 0)
        if ctx.needs_input_grad[3]:
            grad_source = grad_output.gather(ctx.dim, index)
        return grad_input, None, None, grad_source, None


class Repeat(Function):

    @staticmethod
    def forward(ctx, input, repeats):
        ctx.repeats = repeats
        return input.repeat(repeats)

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_output):
        grad_input = grad_output
        for dim, repeat in enumerate(ctx.repeats):
            if repeat == 1:
                continue
            grad_input = sum(grad_input.chunk(repeat, dim))
        return grad_input, None


class Cumsum(Function):

    def __init__(self, dim):
        super(Cumsum, self).__init__()
        self.dim = dim

    def forward(self, input):
        return torch.cumsum(input, dim=self.dim)

    def backward(self, grad_output):
        grad_input = torch.cumsum(-grad_output, dim=self.dim)

        end_idx = grad_input.size(self.dim) - 1
        grad_sum = grad_input.narrow(self.dim, end_idx, 1)
        grad_input -= grad_sum.expand_as(grad_input)
        grad_input += grad_output
        return grad_input


# TODO: unfold
