/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2013, OpenCV Foundation, all rights reserved.
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../precomp.hpp"
#include "layers_common.hpp"
#include "../op_halide.hpp"
#include "../op_inf_engine.hpp"
#include "../op_vkcom.hpp"
#include <opencv2/dnn/shape_utils.hpp>
#include <iostream>

#ifdef HAVE_OPENCL
#include "opencl_kernels_dnn.hpp"
#endif

namespace cv
{
namespace dnn
{

using std::abs;
using std::exp;
using std::tanh;
using std::pow;

template<typename Func>
class ElementWiseLayer : public Func::Layer
{
public:
    class PBody : public cv::ParallelLoopBody
    {
    public:
        const Func* func_;
        const Mat* src_;
        Mat* dst_;
        int nstripes_;

        PBody(const Func &func, const Mat &src, Mat& dst, int nstripes)
        {
            func_ = &func;
            src_ = &src;
            dst_ = &dst;
            nstripes_ = nstripes;
        }

        void operator()(const Range &r) const CV_OVERRIDE
        {
            int nstripes = nstripes_, nsamples = 1, outCn = 1;
            size_t planeSize = 1;

            if (src_->dims > 1)
            {
                nsamples = src_->size[0];
                outCn = src_->size[1];
            }
            else
                outCn = src_->size[0];

            for (int i = 2; i < src_->dims; ++i)
                planeSize *= src_->size[i];

            size_t stripeSize = (planeSize + nstripes - 1)/nstripes;
            size_t stripeStart = r.start*stripeSize;
            size_t stripeEnd = std::min(r.end*stripeSize, planeSize);

            for( int i = 0; i < nsamples; i++ )
            {
                const float* srcptr = src_->ptr<float>(i) + stripeStart;
                float* dstptr = dst_->ptr<float>(i) + stripeStart;
                func_->apply(srcptr, dstptr, (int)(stripeEnd - stripeStart), planeSize, 0, outCn);
            }
        }
    };

    ElementWiseLayer(const Func &f=Func()) : run_parallel(false) { func = f; }

    virtual bool supportBackend(int backendId) CV_OVERRIDE
    {
        return func.supportBackend(backendId, this->preferableTarget);
    }

    virtual Ptr<BackendNode> tryAttach(const Ptr<BackendNode>& node) CV_OVERRIDE
    {
        switch (node->backendId)
        {
            case DNN_BACKEND_HALIDE:
            {
#ifdef HAVE_HALIDE
                auto base = node.dynamicCast<HalideBackendNode>();
                Halide::Func& input = base->funcs.back();
                Halide::Var x("x"), y("y"), c("c"), n("n");
                Halide::Func top = (this->name.empty() ? Halide::Func() : Halide::Func(this->name));
                func.attachHalide(input(x, y, c, n), top);
                return Ptr<BackendNode>(new HalideBackendNode(base, top));
#endif  // HAVE_HALIDE
                break;
            }
        }
        return Ptr<BackendNode>();
    }

    virtual Ptr<BackendNode> initHalide(const std::vector<Ptr<BackendWrapper> > &inputs) CV_OVERRIDE
    {
#ifdef HAVE_HALIDE
        Halide::Buffer<float> input = halideBuffer(inputs[0]);
        Halide::Var x("x"), y("y"), c("c"), n("n");
        Halide::Func top = (this->name.empty() ? Halide::Func() : Halide::Func(this->name));
        func.attachHalide(input(x, y, c, n), top);
        return Ptr<BackendNode>(new HalideBackendNode(top));
#endif  // HAVE_HALIDE
        return Ptr<BackendNode>();
    }

    virtual Ptr<BackendNode> initInfEngine(const std::vector<Ptr<BackendWrapper> >&) CV_OVERRIDE
    {
#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
        InferenceEngine::Builder::Layer ieLayer = func.initInfEngineBuilderAPI();
        ieLayer.setName(this->name);
        return Ptr<BackendNode>(new InfEngineBackendNode(ieLayer));
#else
        InferenceEngine::LayerParams lp;
        lp.name = this->name;
        lp.precision = InferenceEngine::Precision::FP32;
        return Ptr<BackendNode>(new InfEngineBackendNode(func.initInfEngine(lp)));
#endif
#endif  // HAVE_INF_ENGINE
        return Ptr<BackendNode>();
    }

    virtual Ptr<BackendNode> initVkCom(const std::vector<Ptr<BackendWrapper> >& inputs) CV_OVERRIDE
    {
#ifdef HAVE_VULKAN
        return Ptr<BackendNode>(new VkComBackendNode(inputs, func.initVkCom()));
#endif  // HAVE_VULKAN
        return Ptr<BackendNode>();
    }

    virtual bool tryFuse(Ptr<dnn::Layer>& top) CV_OVERRIDE
    {
        return func.tryFuse(top);
    }

    void getScaleShift(Mat& scale_, Mat& shift_) const CV_OVERRIDE
    {
        func.getScaleShift(scale_, shift_);
    }

    bool getMemoryShapes(const std::vector<MatShape> &inputs,
                         const int requiredOutputs,
                         std::vector<MatShape> &outputs,
                         std::vector<MatShape> &internals) const CV_OVERRIDE
    {
        Layer::getMemoryShapes(inputs, requiredOutputs, outputs, internals);
        return true;
    }

    void forward(InputArrayOfArrays inputs_arr, OutputArrayOfArrays outputs_arr, OutputArrayOfArrays internals_arr) CV_OVERRIDE
    {
        CV_TRACE_FUNCTION();

        CV_OCL_RUN(IS_DNN_OPENCL_TARGET(this->preferableTarget),
                   func.applyOCL(inputs_arr, outputs_arr, internals_arr))

        if (inputs_arr.depth() == CV_16S)
        {
            Layer::forward_fallback(inputs_arr, outputs_arr, internals_arr);
            return;
        }

        std::vector<Mat> inputs, outputs;
        inputs_arr.getMatVector(inputs);
        outputs_arr.getMatVector(outputs);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            const Mat &src = inputs[i];
            Mat &dst = outputs[i];
            CV_Assert(src.size == dst.size && src.type() == dst.type() &&
                      src.isContinuous() && dst.isContinuous() && src.type() == CV_32F);

            const int nstripes = getNumThreads();
            PBody body(func, src, dst, nstripes);
            parallel_for_(Range(0, nstripes), body, nstripes);
        }
    }

    void forwardSlice(const float* src, float* dst, int len, size_t planeSize, int cn0, int cn1) const CV_OVERRIDE
    {
        func.apply(src, dst, len, planeSize, cn0, cn1);
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const CV_OVERRIDE
    {
        long flops = 0;
        for (int i = 0; i < outputs.size(); i++)
        {
            flops += total(outputs[i]) * func.getFLOPSPerElement();
        }
        return flops;
    }

    Func func;
    bool run_parallel;
};

#ifdef HAVE_OPENCL
static String oclGetTMacro(const UMat &m)
{
    String str_name = ocl::typeToStr(m.type());

    if (str_name == "short")
        str_name = "half";

    return format("-DT=%s -Dconvert_T=convert_%s ", str_name.c_str(), str_name.c_str());
}
#endif

struct ReLUFunctor
{
    typedef ReLULayer Layer;
    float slope;

    explicit ReLUFunctor(float slope_=1.f) : slope(slope_) {}

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE ||
               backendId == DNN_BACKEND_VKCOM;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        float s = slope;
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            int i = 0;
#if CV_SIMD128
            v_float32x4 s4 = v_setall_f32(s), z = v_setzero_f32();
            for( ; i <= len - 16; i += 16 )
            {
                v_float32x4 x0 = v_load(srcptr + i);
                v_float32x4 x1 = v_load(srcptr + i + 4);
                v_float32x4 x2 = v_load(srcptr + i + 8);
                v_float32x4 x3 = v_load(srcptr + i + 12);
                x0 = v_select(x0 >= z, x0, x0*s4);
                x1 = v_select(x1 >= z, x1, x1*s4);
                x2 = v_select(x2 >= z, x2, x2*s4);
                x3 = v_select(x3 >= z, x3, x3*s4);
                v_store(dstptr + i, x0);
                v_store(dstptr + i + 4, x1);
                v_store(dstptr + i + 8, x2);
                v_store(dstptr + i + 12, x3);
            }
#endif
            for( ; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = x >= 0.f ? x : s*x;
            }
        }
    }

#ifdef HAVE_OPENCL
    bool initKernel(ocl::Kernel &ker, const UMat &src) const
    {
        const char *buildoptSlope = (slope == 0) ? "-DRELU_NO_SLOPE" : "";
        String buildopt = oclGetTMacro(src) + buildoptSlope;

        if (!ker.create("ReLUForward", ocl::dnn::activations_oclsrc, buildopt))
            return false;

        if (slope != 0)
            ker.set(3, (float)slope);

        return true;
    }

    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];
            CV_Assert(src.isContinuous() && dst.isContinuous() && !src.offset && !dst.offset);

            ocl::Kernel kernel;
            CV_Assert(initKernel(kernel, src));
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        if (slope)
        {
            top(x, y, c, n) = select(input >= 0.0f, input, slope * input);
        }
        else
        {
            top(x, y, c, n) = max(input, 0.0f);
        }
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::ReLULayer("").setNegativeSlope(slope);
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "ReLU";
        std::shared_ptr<InferenceEngine::ReLULayer> ieLayer(new InferenceEngine::ReLULayer(lp));
        ieLayer->negative_slope = slope;
        ieLayer->params["negative_slope"] = format("%f", slope);
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        std::shared_ptr<vkcom::OpBase> op(new vkcom::OpReLU(slope));
        return op;
    }
#endif  // HAVE_VULKAN



    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 1; }
};

struct ReLU6Functor
{
    typedef ReLU6Layer Layer;
    float minValue, maxValue;

    ReLU6Functor(float minValue_ = 0.0f, float maxValue_ = 6.0f)
        : minValue(minValue_), maxValue(maxValue_)
    {
        CV_Assert(minValue <= maxValue);
    }

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            int i = 0;
#if CV_SIMD128
            v_float32x4 minV = v_setall_f32(minValue), maxV = v_setall_f32(maxValue);
            for( ; i <= len - 16; i += 16 )
            {
                v_float32x4 x0 = v_load(srcptr + i);
                v_float32x4 x1 = v_load(srcptr + i + 4);
                v_float32x4 x2 = v_load(srcptr + i + 8);
                v_float32x4 x3 = v_load(srcptr + i + 12);
                x0 = v_min(v_max(minV, x0), maxV);
                x1 = v_min(v_max(minV, x1), maxV);
                x2 = v_min(v_max(minV, x2), maxV);
                x3 = v_min(v_max(minV, x3), maxV);
                v_store(dstptr + i, x0);
                v_store(dstptr + i + 4, x1);
                v_store(dstptr + i + 8, x2);
                v_store(dstptr + i + 12, x3);
            }
#endif
            for( ; i < len; i++ )
            {
                float x = srcptr[i];
                if (x >= minValue)
                    dstptr[i] = x <= maxValue ? x : maxValue;
                else
                    dstptr[i] = minValue;
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("ReLU6Forward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));
            kernel.set(3, (float)minValue);
            kernel.set(4, (float)maxValue);

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = clamp(input, minValue, maxValue);
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::ClampLayer("").setMinValue(minValue).setMaxValue(maxValue);
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "Clamp";
        std::shared_ptr<InferenceEngine::ClampLayer> ieLayer(new InferenceEngine::ClampLayer(lp));
        ieLayer->min_value = minValue;
        ieLayer->max_value = maxValue;
        ieLayer->params["min"] = format("%f", minValue);
        ieLayer->params["max"] = format("%f", maxValue);
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 2; }
};

struct TanHFunctor
{
    typedef TanHLayer Layer;

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = tanh(x);
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("TanHForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = tanh(input);
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::TanHLayer("");
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "TanH";
        std::shared_ptr<InferenceEngine::CNNLayer> ieLayer(new InferenceEngine::CNNLayer(lp));
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 1; }
};

struct SigmoidFunctor
{
    typedef SigmoidLayer Layer;

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = 1.f/(1.f + exp(-x));
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("SigmoidForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = 1.0f / (1.0f + exp(-input));
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::SigmoidLayer("");
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "Sigmoid";
        std::shared_ptr<InferenceEngine::CNNLayer> ieLayer(new InferenceEngine::CNNLayer(lp));
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 3; }
};

struct ELUFunctor
{
    typedef ELULayer Layer;

    explicit ELUFunctor() {}

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for(int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = x >= 0.f ? x : exp(x) - 1;
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("ELUForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = select(input >= 0.0f, input, exp(input) - 1);
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::ELULayer("");
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "ELU";
        return InferenceEngine::CNNLayerPtr(new InferenceEngine::CNNLayer(lp));
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 2; }
};

struct AbsValFunctor
{
    typedef AbsLayer Layer;

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = abs(x);
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("AbsValForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = abs(input);
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::ReLULayer("").setNegativeSlope(-1);
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "ReLU";
        std::shared_ptr<InferenceEngine::ReLULayer> ieLayer(new InferenceEngine::ReLULayer(lp));
        ieLayer->negative_slope = -1;
        ieLayer->params["negative_slope"] = "-1.0";
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 1; }
};

struct BNLLFunctor
{
    typedef BNLLLayer Layer;

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = log(1.f + exp(-abs(x)));
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        // TODO: implement OCL version
        return false;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        top(x, y, c, n) = log(1.0f + exp(-abs(input)));
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        CV_Error(Error::StsNotImplemented, "");
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        CV_Error(Error::StsNotImplemented, "BNLL");
        return InferenceEngine::CNNLayerPtr();
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 5; }
};

struct PowerFunctor
{
    typedef PowerLayer Layer;

    float power;
    float scale;
    float shift;

    explicit PowerFunctor(float power_ = 1.f, float scale_ = 1.f, float shift_ = 0.f)
        : power(power_), scale(scale_), shift(shift_) {}

    bool supportBackend(int backendId, int targetId)
    {
        if (backendId == DNN_BACKEND_INFERENCE_ENGINE)
            return (targetId != DNN_TARGET_OPENCL && targetId != DNN_TARGET_OPENCL_FP16) || power == 1.0 || power == 0.5;
        else
            return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        float a = scale, b = shift, p = power;
        if( p == 1.f )
        {
            for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
            {
                for( int i = 0; i < len; i++ )
                {
                    float x = srcptr[i];
                    dstptr[i] = a*x + b;
                }
            }
        }
        else
        {
            for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
            {
                for( int i = 0; i < len; i++ )
                {
                    float x = srcptr[i];
                    dstptr[i] = pow(a*x + b, p);
                }
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("PowForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(2, ocl::KernelArg::PtrWriteOnly(dst));
            kernel.set(3, (float)power);
            kernel.set(4, (float)scale);
            kernel.set(5, (float)shift);

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        Halide::Expr topExpr = (scale == 1.0f ? input : input * scale);
        if (shift)
        {
            topExpr += shift;
        }
        if (power != 1.0f)
        {
            topExpr = pow(topExpr, power);
        }
        top(x, y, c, n) = topExpr;
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        return InferenceEngine::Builder::PowerLayer("").setPower(power)
                                                       .setScale(scale)
                                                       .setShift(shift);
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        if (power == 1.0f && scale == 1.0f && shift == 0.0f)
        {
            // It looks like there is a bug in Inference Engine for DNN_TARGET_OPENCL and DNN_TARGET_OPENCL_FP16
            // if power layer do nothing so we replace it to Identity.
            lp.type = "Split";
            return std::shared_ptr<InferenceEngine::SplitLayer>(new InferenceEngine::SplitLayer(lp));
        }
        else
        {
            lp.type = "Power";
            std::shared_ptr<InferenceEngine::PowerLayer> ieLayer(new InferenceEngine::PowerLayer(lp));
            ieLayer->power = power;
            ieLayer->scale = scale;
            ieLayer->offset = shift;
            return ieLayer;
        }
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>& top)
    {
        if (power != 1.0f && shift != 0.0f)
            return false;

        Mat w, b;
        top->getScaleShift(w, b);
        if ((w.empty() && b.empty()) || w.total() > 1 || b.total() > 1)
            return false;

        float nextScale = w.empty() ? 1.0f : w.at<float>(0);
        float nextShift = b.empty() ? 0.0f : b.at<float>(0);
        scale = std::pow(scale, power) * nextScale;
        shift = nextScale * shift + nextShift;
        return true;
    }

    void getScaleShift(Mat& _scale, Mat& _shift) const
    {
        if (power == 1.0f)
        {
            _scale = Mat(1, 1, CV_32F, Scalar(scale));
            _shift = Mat(1, 1, CV_32F, Scalar(shift));
        }
    }

    int64 getFLOPSPerElement() const { return power == 1 ? 2 : 10; }
};


struct ChannelsPReLUFunctor
{
    typedef ChannelsPReLULayer Layer;
    Mat scale;
#ifdef HAVE_OPENCL
    UMat scale_umat;
#endif

    explicit ChannelsPReLUFunctor(const Mat& scale_=Mat()) : scale(scale_)
    {
    }

    bool supportBackend(int backendId, int)
    {
        return backendId == DNN_BACKEND_OPENCV || backendId == DNN_BACKEND_HALIDE ||
               backendId == DNN_BACKEND_INFERENCE_ENGINE;
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        CV_Assert(scale.isContinuous() && scale.type() == CV_32F);

        const float* scaleptr = scale.ptr<float>();
        CV_Assert( 0 <= cn0 && cn0 < cn1 && cn1 <= (int)scale.total() );

        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            float s = scaleptr[cn];
            int i = 0;
        #if CV_SIMD128
            v_float32x4 s4 = v_setall_f32(s), z = v_setzero_f32();
            for( ; i <= len - 16; i += 16 )
            {
                v_float32x4 x0 = v_load(srcptr + i);
                v_float32x4 x1 = v_load(srcptr + i + 4);
                v_float32x4 x2 = v_load(srcptr + i + 8);
                v_float32x4 x3 = v_load(srcptr + i + 12);
                x0 = v_select(x0 >= z, x0, x0*s4);
                x1 = v_select(x1 >= z, x1, x1*s4);
                x2 = v_select(x2 >= z, x2, x2*s4);
                x3 = v_select(x3 >= z, x3, x3*s4);
                v_store(dstptr + i, x0);
                v_store(dstptr + i + 4, x1);
                v_store(dstptr + i + 8, x2);
                v_store(dstptr + i + 12, x3);
            }
        #endif
            for( ; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = x >= 0.f ? x : s*x;
            }
        }
    }

#ifdef HAVE_OPENCL
    bool applyOCL(InputArrayOfArrays inps, OutputArrayOfArrays outs, OutputArrayOfArrays internals)
    {
        if (scale_umat.empty())
            scale.copyTo(scale_umat);

        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inps.getUMatVector(inputs);
        outs.getUMatVector(outputs);
        String buildopt = oclGetTMacro(inputs[0]);

        for (size_t i = 0; i < inputs.size(); i++)
        {
            UMat& src = inputs[i];
            UMat& dst = outputs[i];

            ocl::Kernel kernel("PReLUForward", ocl::dnn::activations_oclsrc, buildopt);
            kernel.set(0, (int)src.total());
            kernel.set(1, (int)src.size[1]);
            kernel.set(2, (int)total(shape(src), 2));
            kernel.set(3, ocl::KernelArg::PtrReadOnly(src));
            kernel.set(4, ocl::KernelArg::PtrWriteOnly(dst));
            kernel.set(5, ocl::KernelArg::PtrReadOnly(scale_umat));

            size_t gSize = src.total();
            CV_Assert(kernel.run(1, &gSize, NULL, false));
        }

        return true;
    }
#endif

#ifdef HAVE_HALIDE
    void attachHalide(const Halide::Expr& input, Halide::Func& top)
    {
        Halide::Var x("x"), y("y"), c("c"), n("n");
        auto weights = wrapToHalideBuffer(scale, {(int)scale.total()});
        top(x, y, c, n) = select(input >= 0.0f, input, weights(c) * input);
    }
#endif  // HAVE_HALIDE

#ifdef HAVE_INF_ENGINE
#if INF_ENGINE_VER_MAJOR_GE(INF_ENGINE_RELEASE_2018R5)
    InferenceEngine::Builder::Layer initInfEngineBuilderAPI()
    {
        InferenceEngine::Builder::Layer l = InferenceEngine::Builder::PReLULayer("");
        const size_t numChannels = scale.total();
        addConstantData("weights", wrapToInfEngineBlob(scale, {numChannels}, InferenceEngine::Layout::C), l);
        return l;
    }
#else
    InferenceEngine::CNNLayerPtr initInfEngine(InferenceEngine::LayerParams& lp)
    {
        lp.type = "PReLU";
        std::shared_ptr<InferenceEngine::PReLULayer> ieLayer(new InferenceEngine::PReLULayer(lp));
        const size_t numChannels = scale.total();
        ieLayer->_weights = wrapToInfEngineBlob(scale, {numChannels}, InferenceEngine::Layout::C);
        return ieLayer;
    }
#endif
#endif  // HAVE_INF_ENGINE

#ifdef HAVE_VULKAN
    std::shared_ptr<vkcom::OpBase> initVkCom()
    {
        // TODO: add vkcom implementation
        return std::shared_ptr<vkcom::OpBase>();
    }
#endif  // HAVE_VULKAN

    bool tryFuse(Ptr<dnn::Layer>&) { return false; }

    void getScaleShift(Mat&, Mat&) const {}

    int64 getFLOPSPerElement() const { return 1; }
};

#define ACTIVATION_CREATOR_FOR(_Layer, _Functor, ...) \
Ptr<_Layer> _Layer::create() { \
    return return Ptr<_Layer>( new ElementWiseLayer<_Functor>(_Functor()) ); }


Ptr<ReLULayer> ReLULayer::create(const LayerParams& params)
{
    float negativeSlope = params.get<float>("negative_slope", 0.f);
    Ptr<ReLULayer> l(new ElementWiseLayer<ReLUFunctor>(ReLUFunctor(negativeSlope)));
    l->setParamsFrom(params);
    l->negativeSlope = negativeSlope;

    return l;
}

Ptr<ReLU6Layer> ReLU6Layer::create(const LayerParams& params)
{
    float minValue = params.get<float>("min_value", 0.0f);
    float maxValue = params.get<float>("max_value", 6.0f);
    Ptr<ReLU6Layer> l(new ElementWiseLayer<ReLU6Functor>(ReLU6Functor(minValue, maxValue)));
    l->setParamsFrom(params);
    l->minValue = minValue;
    l->maxValue = maxValue;

    return l;
}

Ptr<TanHLayer> TanHLayer::create(const LayerParams& params)
{
    Ptr<TanHLayer> l(new ElementWiseLayer<TanHFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<SigmoidLayer> SigmoidLayer::create(const LayerParams& params)
{
    Ptr<SigmoidLayer> l(new ElementWiseLayer<SigmoidFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<ELULayer> ELULayer::create(const LayerParams& params)
{
    Ptr<ELULayer> l(new ElementWiseLayer<ELUFunctor>(ELUFunctor()));
    l->setParamsFrom(params);

    return l;
}

Ptr<AbsLayer> AbsLayer::create(const LayerParams& params)
{
    Ptr<AbsLayer> l(new ElementWiseLayer<AbsValFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<BNLLLayer> BNLLLayer::create(const LayerParams& params)
{
    Ptr<BNLLLayer> l(new ElementWiseLayer<BNLLFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<PowerLayer> PowerLayer::create(const LayerParams& params)
{
    float power = params.get<float>("power", 1.0f);
    float scale = params.get<float>("scale", 1.0f);
    float shift = params.get<float>("shift", 0.0f);
    Ptr<PowerLayer> l(new ElementWiseLayer<PowerFunctor>(PowerFunctor(power, scale, shift)));
    l->setParamsFrom(params);
    l->power = power;
    l->scale = scale;
    l->shift = shift;

    return l;
}

Ptr<Layer> ChannelsPReLULayer::create(const LayerParams& params)
{
    CV_Assert(params.blobs.size() == 1);
    if (params.blobs[0].total() == 1)
    {
        LayerParams reluParams = params;
        reluParams.set("negative_slope", params.blobs[0].at<float>(0));
        return ReLULayer::create(reluParams);
    }
    Ptr<ChannelsPReLULayer> l(new ElementWiseLayer<ChannelsPReLUFunctor>(ChannelsPReLUFunctor(params.blobs[0])));
    l->setParamsFrom(params);

    return l;
}

}
}
