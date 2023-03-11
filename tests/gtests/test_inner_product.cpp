/*******************************************************************************
* Copyright 2016 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"

#include "mkldnn.hpp"

namespace mkldnn {

struct test_inner_product_descr_t {
    uint32_t mb;
    uint32_t ic;
    uint32_t oc;
    uint32_t kh, kw;
};

template <typename data_t>
void compute_ref_inner_product_fwd(test_inner_product_descr_t ipd, memory &src,
        memory &weights, memory &bias, memory &dst)
{
    data_t *src_data = (data_t *)src.get_data_handle();
    data_t *weights_data = (data_t *)weights.get_data_handle();
    data_t *bias_data = (data_t *)bias.get_data_handle();
    data_t *dst_data = (data_t *)dst.get_data_handle();

    const memory::desc src_d = src.get_primitive_desc().desc();
    const memory::desc weights_d = weights.get_primitive_desc().desc();
    const memory::desc bias_d = bias.get_primitive_desc().desc();
    const memory::desc dst_d = dst.get_primitive_desc().desc();

#pragma omp parallel for collapse(2)
    for (uint32_t n = 0; n < ipd.mb; n++) {
        for (uint32_t oc = 0; oc < ipd.oc; oc++) {
            uint32_t oidx = n * ipd.oc + oc;
            dst_data[map_index(dst_d, oidx)] = bias_data ?
                    bias_data[map_index(bias_d, oc)] : 0.0;
            for (uint32_t ic = 0; ic < ipd.ic; ic++) {
                for (uint32_t kh = 0; kh < ipd.kh; kh++) {
                    for (uint32_t kw = 0; kw < ipd.kw; kw++) {
                        uint32_t iidx = n * ipd.ic * ipd.kh * ipd.kw
                                + ic * ipd.kh * ipd.kw + kh * ipd.kw + kw;
                        uint32_t widx = oc * ipd.ic * ipd.kh * ipd.kw
                                + ic * ipd.kh * ipd.kw + kh * ipd.kw + kw;
                        dst_data[map_index(dst_d, oidx)]
                                += src_data[map_index(src_d, iidx)]
                                * weights_data[map_index(weights_d, widx)];
                    }
                }
            }
        }
    }
}

struct inprod_test_params {
    prop_kind aprop_kind;
    const engine::kind engine_kind;
    memory::format src_format;
    memory::format weights_format;
    memory::format bias_format;
    memory::format dst_format;
    test_inner_product_descr_t test_ipd;
};

template <typename data_t>
class inner_product_test : public ::testing::TestWithParam<inprod_test_params> {
protected:
    virtual void SetUp()
    {
        inprod_test_params p
                = ::testing::TestWithParam<inprod_test_params>::GetParam();
        test_inner_product_descr_t ipd = p.test_ipd;
        bool has_spatial = ipd.kh > 1 && ipd.kw > 1;

        ASSERT_TRUE(p.engine_kind == engine::kind::cpu
                || p.engine_kind == engine::kind::cpu_lazy);
        ASSERT_EQ(p.aprop_kind, prop_kind::forward);
        auto eng = engine(p.engine_kind, 0);
        memory::precision prec = data_traits<data_t>::prec;
        ASSERT_EQ(prec, mkldnn::memory::precision::f32);

        auto ip_src_desc = has_spatial ?
                create_md({ ipd.mb, ipd.ic, ipd.kh, ipd.kw }, prec,
                        p.src_format) :
                create_md({ ipd.mb, ipd.ic }, prec, p.src_format);
        auto ip_weights_desc = has_spatial ?
                create_md({ ipd.oc, ipd.ic, ipd.kh, ipd.kw }, prec,
                        p.weights_format) :
                create_md({ ipd.oc, ipd.ic }, prec, p.weights_format);
        auto ip_bias_desc = create_md({ ipd.oc }, prec, p.bias_format);
        auto ip_dst_desc = create_md({ ipd.mb, ipd.oc }, prec, p.dst_format);

        auto ip_src = memory(memory::primitive_desc(ip_src_desc, eng));
        auto ip_weights = memory(memory::primitive_desc(ip_weights_desc, eng));
        auto ip_bias = memory(memory::primitive_desc(ip_bias_desc, eng));
        auto ip_dst = memory(memory::primitive_desc(ip_dst_desc, eng));
        auto dst_ref = memory(memory::primitive_desc(ip_dst_desc, eng));

        fill_data<data_t>(ip_src.get_primitive_desc().get_number_of_elements(),
                (data_t *)ip_src.get_data_handle());
        fill_data<data_t>(
                ip_weights.get_primitive_desc().get_number_of_elements(),
                (data_t *)ip_weights.get_data_handle());
        fill_data<data_t>(ip_bias.get_primitive_desc().get_number_of_elements(),
                (data_t *)ip_bias.get_data_handle());

        auto ip = inner_product(p.aprop_kind, ip_src, ip_weights, ip_bias,
                ip_dst);

        std::vector<primitive> pipeline;
        pipeline.push_back(ip);

        stream().submit(pipeline).wait();

        compute_ref_inner_product_fwd<data_t>(ipd, ip_src, ip_weights, ip_bias,
                dst_ref);
        compare_data<data_t>(dst_ref, ip_dst);
    }
};

using inner_product_test_float = inner_product_test<float>;
using inprod_test_params_float = inprod_test_params;

TEST_P(inner_product_test_float, TestsInnerProduct)
{
}
INSTANTIATE_TEST_CASE_P(
        TestInnerProductForward, inner_product_test_float,
        ::testing::Values(
                inprod_test_params_float{ prop_kind::forward, engine::kind::cpu,
                        memory::format::nchw, memory::format::oihw,
                        memory::format::x, memory::format::nc,
                        { 2, 32, 48, 6, 6 } },
                inprod_test_params_float{ prop_kind::forward, engine::kind::cpu,
                        memory::format::nChw8c, memory::format::oIhw8i,
                        memory::format::x, memory::format::nc,
                        { 2, 32, 48, 6, 6 } },
                inprod_test_params_float{ prop_kind::forward, engine::kind::cpu,
                        memory::format::nc, memory::format::oi,
                        memory::format::x, memory::format::nc,
                        { 2, 32, 1152, 1, 1 } },
                inprod_test_params_float{ prop_kind::forward, engine::kind::cpu,
                        memory::format::nc, memory::format::oi,
                        memory::format::x, memory::format::nc,
                        { 2, 2, 4, 1, 1 } }));
}
