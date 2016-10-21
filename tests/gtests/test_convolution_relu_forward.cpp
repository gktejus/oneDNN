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

struct test_convolution_relu_descr_t {
    int mb;
    int ng;
    int ic, ih, iw;
    int oc, oh, ow;
    int kh, kw;
    int padh, padw;
    int strh, strw;
    double negative_slope;
};

template <typename data_t>
void compute_ref_conv_relu_fwd(const test_convolution_relu_descr_t &c,
        const memory &src, const memory &weights, const memory &bias,
        const memory &dst, bool w_bias)
{
    data_t *src_data = (data_t *)src.get_data_handle();
    data_t *weights_data = (data_t *)weights.get_data_handle();
    data_t *bias_data
            = (data_t *)(w_bias ? bias.get_data_handle() : nullptr);
    data_t *dst_data = (data_t *)dst.get_data_handle();

    const memory::desc src_d = src.get_primitive_desc().desc();
    const memory::desc weights_d = weights.get_primitive_desc().desc();
    const memory::desc dst_d = dst.get_primitive_desc().desc();

#pragma omp parallel for collapse(5) schedule(static)
    for (int n = 0; n < c.mb; n++) {
        for (int g = 0; g < c.ng; g++) {
            for (int oc = 0; oc < c.oc / c.ng; oc++) {
                for (int oh = 0; oh < c.oh; oh++) {
                    for (int ow = 0; ow < c.ow; ow++) {
                        int oidx = n * c.oc * c.oh * c.ow
                                + g * c.oc / c.ng * c.oh * c.ow
                                + oc * c.oh * c.ow + oh * c.ow + ow;
                        dst_data[map_index(dst_d, oidx)] = bias_data ?
                                bias_data[map_index(
                                        bias.get_primitive_desc().desc(),
                                        g * c.oc / c.ng + oc)] :
                                0.0;
                        for (int ic = 0; ic < c.ic / c.ng; ic++) {
                            for (int kh = 0; kh < c.kh; kh++) {
                                for (int kw = 0; kw < c.kw; kw++) {
                                    int iw = ow * c.strw - c.padw + kw;
                                    int ih = oh * c.strh - c.padh + kh;
                                    if (iw < 0 || iw >= c.iw) continue;
                                    if (ih < 0 || ih >= c.ih) continue;
                                    int iidx = n * c.ic * c.ih * c.iw
                                            + g * c.ic / c.ng * c.ih * c.iw
                                            + ic * c.ih * c.iw + ih * c.iw + iw;
                                    int widx = g * c.oc / c.ng * c.ic
                                                    / c.ng * c.kh * c.kw
                                            + oc * c.ic / c.ng * c.kh * c.kw
                                            + ic * c.kh * c.kw + kh * c.kw + kw;

                                    dst_data[map_index(dst_d, oidx)]
                                            += src_data[map_index(src_d, iidx)]
                                            * weights_data[map_index(
                                                      weights_d, widx)];
                                }
                            }
                        }

                        if (dst_data[map_index(dst_d, oidx)] < 0) {
                            dst_data[map_index(dst_d, oidx)] *=
                                c.negative_slope;
                        }

                    }
                }
            }
        }
    }
}

struct conv_relu_test_params {
    prop_kind aprop_kind;
    const engine::kind engine_kind;
    algorithm aalgorithm;
    memory::format src_format;
    memory::format weights_format;
    memory::format bias_format;
    memory::format dst_format;
    test_convolution_relu_descr_t test_cd;
};

template <typename data_t>
class convolution_relu_test : public ::testing::TestWithParam<conv_relu_test_params> {
protected:
    virtual void SetUp()
    {
        conv_relu_test_params p
                = ::testing::TestWithParam<conv_relu_test_params>::GetParam();

        ASSERT_TRUE(p.engine_kind == engine::kind::cpu);
        ASSERT_EQ(p.aprop_kind, prop_kind::forward_scoring);
        ASSERT_EQ(p.aalgorithm, convolution_direct);
        auto eng = engine(p.engine_kind, 0);
        memory::data_type data_type = data_traits<data_t>::data_type;
        ASSERT_EQ(data_type, mkldnn::memory::data_type::f32);

        test_convolution_relu_descr_t cd = p.test_cd;

        auto c_src_desc
                = create_md({ cd.mb, cd.ic, cd.ih, cd.iw }, data_type, p.src_format);
        auto c_weights_desc = cd.ng > 1 ?
                create_md({ cd.ng, cd.oc / cd.ng, cd.ic / cd.ng, cd.kh, cd.kw },
                        data_type, p.weights_format) :
                create_md(
                        { cd.oc, cd.ic, cd.kh, cd.kw }, data_type, p.weights_format);
        auto c_dst_desc
                = create_md({ cd.mb, cd.oc, cd.oh, cd.ow }, data_type, p.dst_format);

        auto c_src = memory({c_src_desc, eng});
        auto c_weights = memory({c_weights_desc, eng});
        auto c_dst = memory({c_dst_desc, eng});

        auto dst_ref = memory({c_dst_desc, eng});

        fill_data<data_t>(c_src.get_primitive_desc().get_size() / sizeof(data_t),
                (data_t *)c_src.get_data_handle());

        fill_data<data_t>(
                c_weights.get_primitive_desc().get_size() / sizeof(data_t),
                (data_t *)c_weights.get_data_handle());

        bool with_bias = p.bias_format != memory::format::format_undef;
        auto c_bias_desc = with_bias ?
                create_md({ cd.oc }, data_type, p.bias_format) :
                create_md({}, data_type, p.bias_format);
        auto c_bias = memory({c_bias_desc, eng});
        if (with_bias) {
            fill_data<data_t>(
                    c_bias.get_primitive_desc().get_size() / sizeof(data_t),
                    (data_t *)c_bias.get_data_handle());
        }

        std::vector<int> padR = { cd.padh, cd.padw };
        for (int i = 0; i < 2; ++i) {
        if ((cd.ih + cd.padh + padR[0] - cd.kh)/cd.strh + 1 != cd.oh) ++padR[0];
        if ((cd.iw + cd.padw + padR[1] - cd.kw)/cd.strw + 1 != cd.ow) ++padR[1];
        }

        auto conv_desc = with_bias ?
                convolution_forward::desc(p.aprop_kind, p.aalgorithm,
                        c_src_desc, c_weights_desc, c_bias_desc, c_dst_desc,
                        { cd.strh, cd.strw }, { cd.padh, cd.padw }, padR,
                        padding_kind::zero) :
                convolution_forward::desc(p.aprop_kind, p.aalgorithm,
                        c_src_desc, c_weights_desc, c_dst_desc,
                        { cd.strh, cd.strw }, { cd.padh, cd.padw }, padR,
                        padding_kind::zero);

        auto conv_relu_desc =
            convolution_relu_forward::desc(conv_desc, cd.negative_slope);
        auto conv_primitive_desc = convolution_relu_forward::primitive_desc(
                conv_relu_desc, eng);

        auto conv = with_bias ?
            convolution_relu_forward(conv_primitive_desc,
                    c_src, c_weights, c_bias, c_dst) :
            convolution_relu_forward(conv_primitive_desc,
                    c_src, c_weights, c_dst);
        std::vector<primitive> pipeline;
        pipeline.push_back(conv);

        stream(stream::kind::lazy).submit(pipeline).wait();

        compute_ref_conv_relu_fwd<data_t>(cd, c_src, c_weights, c_bias,
            dst_ref, with_bias);
        compare_data<data_t>(dst_ref, c_dst);
    }
};

using convolution_relu_test_float = convolution_relu_test<float>;
using conv_relu_test_params_float = conv_relu_test_params;

TEST_P(convolution_relu_test_float, TestsConvolution)
{
}
INSTANTIATE_TEST_CASE_P(
        TestConvolutionForward, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 1, 4, 4, 4, 6, 4, 4, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 1, 4, 4, 4, 6, 2, 2, 3, 3, 0, 0, 1, 1, 0.0 } }));

INSTANTIATE_TEST_CASE_P(
        TestConvolutionForwardNoBias, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::format_undef,
                        memory::format::nchw,
                        { 2, 1, 4, 4, 4, 6, 4, 4, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::format_undef,
                        memory::format::nchw,
                        { 2, 1, 4, 4, 4, 6, 2, 2, 3, 3, 0, 0, 1, 1, 0.0 } }));

INSTANTIATE_TEST_CASE_P(
        TestConvolutionForwardNHWC, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nhwc,
                        memory::format::oihw, memory::format::x,
                        memory::format::nhwc,
                        { 2, 1, 4, 4, 4, 6, 4, 4, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nhwc,
                        memory::format::oihw, memory::format::x,
                        memory::format::nhwc,
                        { 2, 1, 4, 4, 4, 6, 2, 2, 3, 3, 0, 0, 1, 1, 0.0 } }));
INSTANTIATE_TEST_CASE_P(
        TestConvolutionForwardBlocked, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::OIhw8i8o, memory::format::x,
                        memory::format::nChw8c,
                        { 2, 1, 32, 13, 13, 48, 13, 13, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::OIhw8i8o, memory::format::x,
                        memory::format::nChw8c,
                        { 2, 1, 32, 13, 13, 48, 11, 11, 3, 3, 0, 0, 1, 1, 0.0 } }));
INSTANTIATE_TEST_CASE_P(
        TestConvolutionAlexnetForwardNCHW, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 1, 3, 227, 227, 96, 55, 55, 11, 11, 0, 0, 4, 4, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::goihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 2, 96, 27, 27, 256, 27, 27, 5, 5, 2, 2, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::oihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 1, 256, 13, 13, 384, 13, 13, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::goihw, memory::format::x,
                        memory::format::nchw,
                        { 2, 2, 384, 13, 13, 384, 13, 13, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::goihw, memory::format::x,
                        memory::format::nchw, { 2, 2, 384, 13, 13, 256, 13, 13,
                                                      3, 3, 1, 1, 1, 1, 0.0 } }));
INSTANTIATE_TEST_CASE_P(
        TestConvolutionAlexnetForwardBlocked, convolution_relu_test_float,
        ::testing::Values(
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nchw,
                        memory::format::Ohwi8o, memory::format::x,
                        memory::format::nChw8c,
                        { 2, 1, 3, 227, 227, 96, 55, 55, 11, 11, 0, 0, 4, 4, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nhwc,
                        memory::format::Ohwi8o, memory::format::x,
                        memory::format::nChw8c,
                        { 2, 1, 3, 227, 227, 96, 55, 55, 11, 11, 0, 0, 4, 4, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::gOIhw8i8o, memory::format::x,
                        memory::format::nChw8c,
                        { 2, 2, 96, 27, 27, 256, 27, 27, 5, 5, 2, 2, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::OIhw8i8o, memory::format::format_undef,
                        memory::format::nChw8c,
                        { 2, 1, 256, 13, 13, 384, 13, 13, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::gOIhw8i8o, memory::format::format_undef,
                        memory::format::nChw8c,
                        { 2, 2, 384, 13, 13, 384, 13, 13, 3, 3, 1, 1, 1, 1, 0.0 } },
                conv_relu_test_params_float{ prop_kind::forward_scoring, engine::kind::cpu,
                        convolution_direct, memory::format::nChw8c,
                        memory::format::gOIhw8i8o, memory::format::format_undef,
                        memory::format::nChw8c,
                        { 2, 2, 384, 13, 13, 256, 13, 13, 3, 3, 1, 1, 1,
                            1, 0.0 } }));
}
