//*****************************************************************************
// Copyright (c) 2016-2020, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// - Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// - Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//*****************************************************************************

#include <iostream>

#include <backend_iface.hpp>
#include "backend_fptr.hpp"
#include "backend_utils.hpp"
#include "queue_sycl.hpp"

namespace mkl_dft = oneapi::mkl::dft;

template <typename _KernelNameSpecialization1, typename _KernelNameSpecialization2>
class dpnp_fft_fft_c_kernel;

template <typename _DataType_input, typename _DataType_output>
void dpnp_fft_fft_c(const void* array1_in,
                    void* result1,
                    const long* input_shape,
                    const long* output_shape,
                    size_t shape_size,
                    long axis,
                    long input_boundarie,
                    size_t inverse)
{
    const size_t result_size = std::accumulate(output_shape, output_shape + shape_size, 1, std::multiplies<size_t>());
    if (!(result_size && shape_size))
    {
        return;
    }

    cl::sycl::event event;
    const double kernel_pi = inverse ? -M_PI : M_PI;

    const _DataType_input* array_1 = reinterpret_cast<const _DataType_input*>(array1_in);
    _DataType_output* result = reinterpret_cast<_DataType_output*>(result1);

    // kernel specific temporal data
    long* output_shape_offsets = reinterpret_cast<long*>(dpnp_memory_alloc_c(shape_size * sizeof(long)));
    long* input_shape_offsets = reinterpret_cast<long*>(dpnp_memory_alloc_c(shape_size * sizeof(long)));
    // must be a thread local storage.
    long* xyz = reinterpret_cast<long*>(dpnp_memory_alloc_c(result_size * shape_size * sizeof(long)));
    long* axis_iterator = reinterpret_cast<long*>(dpnp_memory_alloc_c(result_size * shape_size * sizeof(long)));

    get_shape_offsets_inkernel<long>(output_shape, shape_size, output_shape_offsets);
    get_shape_offsets_inkernel<long>(input_shape, shape_size, input_shape_offsets);

    cl::sycl::range<1> gws(result_size);
    auto kernel_parallel_for_func = [=](cl::sycl::id<1> global_id) {
        size_t output_id = global_id[0];

        double sum_real = 0.0;
        double sum_imag = 0.0;
        // need to replace these arrays by thread local storage
        long* xyz_thread = xyz + (output_id * shape_size);
        long* axis_iterator_thread = axis_iterator + (output_id * shape_size);

        get_xyz_by_id_inkernel(output_id, output_shape_offsets, shape_size, xyz_thread);
        for (size_t i = 0; i < shape_size; ++i)
        {
            axis_iterator_thread[i] = xyz_thread[i];
        }

        const long axis_length = input_boundarie;
        for (long it = 0; it < axis_length; ++it)
        {
            double in_real = 0.0;
            double in_imag = 0.0;

            axis_iterator_thread[axis] = it;

            const size_t input_it = get_id_by_xyz_inkernel(axis_iterator_thread, shape_size, input_shape_offsets);

            if (it < input_shape[axis])
            {
                if constexpr (std::is_same<_DataType_input, std::complex<double>>::value)
                {
                    const _DataType_input* cmplx_ptr = array_1 + input_it;
                    const double* dbl_ptr = reinterpret_cast<const double*>(cmplx_ptr);
                    in_real = *dbl_ptr;
                    in_imag = *(dbl_ptr + 1);
                }
                else
                {
                    in_real = array_1[input_it];
                }
            }

            const size_t output_local_id = xyz_thread[axis];
            const double angle = 2.0 * kernel_pi * it * output_local_id / axis_length;

            const double angle_cos = cl::sycl::cos(angle);
            const double angle_sin = cl::sycl::sin(angle);

            sum_real += in_real * angle_cos + in_imag * angle_sin;
            sum_imag += -in_real * angle_sin + in_imag * angle_cos;
        }

        if (inverse)
        {
            sum_real = sum_real / input_boundarie;
            sum_imag = sum_imag / input_boundarie;
        }

        result[output_id] = _DataType_output(sum_real, sum_imag);
    };

    auto kernel_func = [&](cl::sycl::handler& cgh) {
        cgh.parallel_for<class dpnp_fft_fft_c_kernel<_DataType_input, _DataType_output>>(gws, kernel_parallel_for_func);
    };

    event = DPNP_QUEUE.submit(kernel_func);

#if 0 // keep this code
    oneapi::mkl::dft::descriptor<mkl_dft::precision::DOUBLE, mkl_dft::domain::COMPLEX> desc(result_size);
    desc.set_value(mkl_dft::config_param::FORWARD_SCALE, static_cast<double>(result_size));
    desc.set_value(mkl_dft::config_param::PLACEMENT, DFTI_NOT_INPLACE); // enum value from MKL C interface
    desc.commit(DPNP_QUEUE);

    event = mkl_dft::compute_forward(desc, array_1, result);
#endif

    event.wait();

    dpnp_memory_free_c(input_shape_offsets);
    dpnp_memory_free_c(output_shape_offsets);
    dpnp_memory_free_c(axis_iterator);
    dpnp_memory_free_c(xyz);

    return;
}

void func_map_init_fft_func(func_map_t& fmap)
{
    fmap[DPNPFuncName::DPNP_FN_FFT_FFT][eft_INT][eft_INT] = {eft_C128,
                                                             (void*)dpnp_fft_fft_c<int, std::complex<double>>};
    fmap[DPNPFuncName::DPNP_FN_FFT_FFT][eft_LNG][eft_LNG] = {eft_C128,
                                                             (void*)dpnp_fft_fft_c<long, std::complex<double>>};
    fmap[DPNPFuncName::DPNP_FN_FFT_FFT][eft_FLT][eft_FLT] = {eft_C128,
                                                             (void*)dpnp_fft_fft_c<float, std::complex<double>>};
    fmap[DPNPFuncName::DPNP_FN_FFT_FFT][eft_DBL][eft_DBL] = {eft_C128,
                                                             (void*)dpnp_fft_fft_c<double, std::complex<double>>};
    fmap[DPNPFuncName::DPNP_FN_FFT_FFT][eft_C128][eft_C128] = {
        eft_C128, (void*)dpnp_fft_fft_c<std::complex<double>, std::complex<double>>};
    return;
}
