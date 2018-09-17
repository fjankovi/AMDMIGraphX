#ifndef MIGRAPH_GUARD_RTGLIB_DEVICE_NARY_HPP
#define MIGRAPH_GUARD_RTGLIB_DEVICE_NARY_HPP

#include <migraph/gpu/device/tensor.hpp>
#include <migraph/gpu/device/launch.hpp>
#include <migraph/functional.hpp>
#include <migraph/ranges.hpp>

namespace migraph {
namespace gpu {
namespace device {

template <class T>
using vec4 = T __attribute__((ext_vector_type(4)));

template <class T>
__device__ __host__ vec4<T>* as_vec4(T* x)
{
    return reinterpret_cast<vec4<T>*>(x);
}

template <class T>
__device__ __host__ T* as_pointer(vec4<T>* x)
{
    return reinterpret_cast<T*>(x);
}

template <class... Ts>
auto pack_vec4(Ts... xs)
{
    return [=](auto f, std::size_t n) { return f(as_vec4(xs)[n]...); };
}

template <class F, class... Arguments>
auto nary_nonstandard_impl(F f, argument result, Arguments... args)
{
    const auto& output_shape = result.get_shape();
    visit_all(result, args...)([&](auto output, auto... inputs) {
        visit_tensor_size(output_shape.lens().size(), [&](auto ndim) {
            auto data = pack(
                std::make_pair(hip_tensor_descriptor<ndim>{inputs.get_shape()}, inputs.data())...);
            hip_tensor_descriptor<ndim> out_desc(output_shape);
            auto* outp = output.data();
            gs_launch(output_shape.elements())([=](auto i) {
                data([&](auto&&... ps) {
                    auto outidx = out_desc.multi(i);
                    outp[i]     = f(ps.second[ps.first.linear(outidx)]...);
                });
            });
        });
    });
}

template <class F>
void binary_broadcast_vec_impl(F f,
                               const argument& result,
                               const argument& arg1,
                               const argument& arg2)
{
    const auto& output_shape = result.get_shape();
    const auto& b_shape      = arg2.get_shape();
    auto bdim =
        std::distance(b_shape.strides().begin(),
                      std::find_if(b_shape.strides().begin(), b_shape.strides().end(), [](auto x) {
                          return x != 0;
                      }));
    auto bdim_len         = output_shape.lens()[bdim];
    auto bdim_stride      = output_shape.strides()[bdim];
    auto bdim_next_stride = bdim_stride * bdim_len;

    visit_all(result, arg1, arg2)([&](auto output, auto input1, auto input2) {
        using type = std::remove_cv_t<typename decltype(output)::value_type>;
        auto* xp   = as_vec4(input1.data());
        auto* yp   = as_vec4(input2.data());
        auto* outp = as_vec4(output.data());

        const std::size_t vec_size     = 4;
        const std::size_t nlocal       = 1024;
        const std::size_t nglobal      = 256 * nlocal;
        const std::size_t n            = output.size() / vec_size;
        const std::size_t bdim_vec_len = bdim_len / vec_size;

        launch(nglobal, nlocal)([=](auto idx) __device__ {
            MIGRAPH_DEVICE_SHARED vec4<type> buffer[2048 / vec_size];
            // Load bias into LDS
            for(size_t i = idx.local; i < bdim_vec_len; i += nlocal)
            {
                buffer[i] = yp[i];
            }
            __syncthreads();
            auto* bp = as_pointer(buffer);
            // Process the data
            for(size_t i = idx.global; i < n; i += nglobal)
            {
                auto bidx      = ((i * vec_size) % bdim_next_stride) / bdim_stride;
                auto b         = bp[bidx];
                vec4<type> x   = xp[i];
                vec4<type> out = outp[i];
                for(std::size_t j = 0; j < vec_size; j++)
                {
                    out[j] = f(x[j], b);
                }
                outp[i] = out;
            }
        });
    });
}

template <class F>
void binary_broadcast_impl(F f, const argument& result, const argument& arg1, const argument& arg2)
{
    const auto& output_shape = result.get_shape();
    const auto& b_shape      = arg2.get_shape();
    auto bdim =
        std::distance(b_shape.strides().begin(),
                      std::find_if(b_shape.strides().begin(), b_shape.strides().end(), [](auto x) {
                          return x != 0;
                      }));
    auto bdim_len         = output_shape.lens()[bdim];
    auto bdim_stride      = output_shape.strides()[bdim];
    auto bdim_next_stride = bdim_stride * bdim_len;

    visit_all(result, arg1, arg2)([&](auto output, auto input1, auto input2) {
        using type = std::remove_cv_t<typename decltype(output)::value_type>;
        auto* xp   = input1.data();
        auto* yp   = input2.data();
        auto* outp = output.data();

        const std::size_t nlocal  = 1024;
        const std::size_t nglobal = 256 * nlocal;
        const std::size_t n       = output.size();

        launch(nglobal, nlocal)([=](auto idx) __device__ {
            MIGRAPH_DEVICE_SHARED type buffer[2048];
            // Load bias into LDS
            for(size_t i = idx.local; i < bdim_len; i += nlocal)
            {
                buffer[i] = yp[i];
            }
            __syncthreads();
            // Process the data
            for(size_t i = idx.global; i < n; i += nglobal)
            {
                auto bidx = (i % bdim_next_stride) / bdim_stride;
                auto b    = buffer[bidx];
                type x    = xp[i];
                outp[i]   = f(x, b);
            }
        });
    });
}

template <class F, class... Arguments>
void nary_standard_vec_impl(F f, argument result, Arguments... args)
{
    // assert(x.get_shape().elements() == y.get_shape().elements());
    const auto& output_shape = result.get_shape();
    visit_all(result, args...)([&](auto output, auto... inputs) {
        using type                 = std::remove_cv_t<typename decltype(output)::value_type>;
        const std::size_t vec_size = 4;
        auto data                  = pack_vec4(inputs.data()...);
        auto* outp                 = as_vec4(output.data());
        gs_launch(output_shape.elements() / vec_size)([=](auto i) {
            vec4<type> out = outp[i];
            data(
                [&](auto... xs) {
                    for(std::size_t j = 0; j < vec_size; j++)
                    {
                        out[j] = f(xs[j]...);
                    }
                },
                i);
            outp[i] = out;
        });
    });
}

template <class F, class... Arguments>
void nary_standard_impl(F f, argument result, Arguments... args)
{
    // assert(x.get_shape().elements() == y.get_shape().elements());
    const auto& output_shape = result.get_shape();
    visit_all(result, args...)([&](auto output, auto... inputs) {
        auto data  = pack(inputs.data()...);
        auto* outp = output.data();
        gs_launch(output_shape.elements())(
            [=](auto i) { data([&](auto... xps) { outp[i] = f(xps[i]...); }); });
    });
}

template <class F, class... Arguments>
void nary_impl(F f, argument result, Arguments... args)
{
    bool standard = all_of({args.get_shape()...}, [](const shape& s) { return s.standard(); });
    bool packed   = all_of({args.get_shape()...}, [](const shape& s) { return s.packed(); });
    bool same_shapes =
        all_of({args.get_shape()...}, [&](const shape& s) { return s == result.get_shape(); });
    if(standard or (packed and same_shapes))
        nary_standard_impl(f, result, args...);
    else
        nary_nonstandard_impl(f, result, args...);
}

template <class... Arguments>
auto nary_nonstandard(argument result, Arguments... args)
{
    return [=](auto f) { nary_nonstandard_impl(f, result, args...); };
}

template <class... Arguments>
auto nary_standard(argument result, Arguments... args)
{
    return [=](auto f) { nary_standard_impl(f, result, args...); };
}

template <class... Arguments>
auto nary(argument result, Arguments... args)
{
    return [=](auto f) { nary_impl(f, result, args...); };
}

inline auto nary(const argument& result, const argument& arg1, const argument& arg2)
{
    return [=](auto f) {
        // TODO: Check result and arg1 shape is the same
        if(arg1.get_shape().standard() and arg2.get_shape().broadcasted())
        {
            auto not_zero       = [](auto x) { return x != 0; };
            const auto& strides = arg2.get_shape().strides();
            auto b_it           = std::find_if(strides.begin(), strides.end(), not_zero);
            auto b_idx          = std::distance(strides.begin(), b_it);
            auto b_len          = result.get_shape().lens()[b_idx];
            auto b_stride       = result.get_shape().strides()[b_idx];
            assert(arg2.get_shape().lens()[b_idx] == b_len);
            if(b_len <= 2048 and std::none_of(std::next(b_it), strides.end(), not_zero))
            {
                const bool divisible_by_4 = (b_len % 4 == 0) and (b_stride % 4 == 0) and
                                            (arg1.get_shape().elements() % 4 == 0);
                if(divisible_by_4)
                    binary_broadcast_vec_impl(f, result, arg1, arg2);
                else
                    binary_broadcast_impl(f, result, arg1, arg2);
                return;
            }
        }
        nary_impl(f, result, arg1, arg2);
    };
}

} // namespace device
} // namespace gpu
} // namespace migraph

#endif