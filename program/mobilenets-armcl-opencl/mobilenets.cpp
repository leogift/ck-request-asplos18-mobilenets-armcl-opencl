/*
 * Copyright (c) 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "benchmark.h"

#include "arm_compute/runtime/CL/CLTensor.h"

class CKNumPyInputLoader : public ITensorAccessor {
public:
  CKNumPyInputLoader() {}
  CKNumPyInputLoader(CKNumPyInputLoader &&) = default;

  bool access_tensor(ITensor &tensor) override {
    CKPredictionSession& s = session();
    auto batch_file = s.get_next_batch_file();
    if (batch_file.empty())
      return false;
      
    cout << endl;
    cout << "Batch " << s.batch_index()+1 << " of " << s.batch_count() << endl;
    cout << "File: " << batch_file << endl;
    
    s.measure_begin();

    NumPyBinLoader accessor(batch_file);
    accessor.access_tensor(tensor);

    auto t = s.measure_end_load_images();
    cout << "Loaded in " << t << " s\n";
    
    // Start batch timer after data was loaded
    s.measure_begin();
    return true;
  }
};


class CKOutputAccessor : public ITensorAccessor {
public:
  CKOutputAccessor() {}
  CKOutputAccessor(CKOutputAccessor &&) = default;

  bool access_tensor(ITensor &tensor) override {
    // Stop batch timer before processing results
    CKPredictionSession& s = session();
    auto t = s.measure_end_prediction();
    cout << "Classified in " << t << "s \n";

    // TODO: some additional work will be required when batch_size > 1 is allowed.
    // We will have to split batch result into a set of results for different images.
    string img_file = s.image_files()[s.batch_index()];
    string res_dir = get_result_dir();
    string res_file = res_dir + path_separator() + img_file + ".txt";
    ofstream f(res_file);

    const size_t num_classes = tensor.info()->dimension(0);
    float* probes  = reinterpret_cast<float*>(tensor.buffer() + tensor.info()->offset_first_element_in_bytes());
    
    // Take off the first probe as it references to 'background' class but no such one in ImageNet
    for (size_t i = 1; i < num_classes; i++)
      f << probes[i] << endl;
      
    return true;
  }
};


namespace
{
inline unique_ptr<ITensorAccessor> weights_accessor(const string &file)
{
    const string path = get_weights_path();
    string full_path = path + path_separator() + file;
    if (!file_exists(full_path))
    {
       cerr << "WARNING: file not found: " << full_path << ", dummy accessor will be used!\n";
       return arm_compute::support::cpp14::make_unique<DummyAccessor>();
    }
    return arm_compute::support::cpp14::make_unique<NumPyBinLoader>(full_path);
}

inline unique_ptr<ITensorAccessor> empty_accessor() {
    return std::unique_ptr<ITensorAccessor>(nullptr);
}


unsigned int apply_multiplier(unsigned int size) {
    return static_cast<unsigned int>(size * get_multiplier());
}

BranchLayer get_dwsc_node(std::string &&param_path,
                          unsigned int  conv_filt,
                          PadStrideInfo dwc_pad_stride_info, PadStrideInfo conv_pad_stride_info)
{
    conv_filt = apply_multiplier(conv_filt);
    std::string total_path = param_path + "_";
    SubGraph    sg;
    sg << DepthwiseConvolutionLayer(
           3U, 3U,
           weights_accessor(total_path + "depthwise_depthwise_weights.npy"),
           empty_accessor(),
           dwc_pad_stride_info,
           true)
       << BatchNormalizationLayer(
           weights_accessor(total_path + "depthwise_BatchNorm_moving_mean.npy"),
           weights_accessor(total_path + "depthwise_BatchNorm_moving_variance.npy"),
           weights_accessor(total_path + "depthwise_BatchNorm_gamma.npy"),
           weights_accessor(total_path + "depthwise_BatchNorm_beta.npy"),
           0.001f)
       << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::BOUNDED_RELU, 6.f))
       << ConvolutionLayer(
           1U, 1U, conv_filt,
           weights_accessor(total_path + "pointwise_weights.npy"),
           empty_accessor(),
           conv_pad_stride_info)
       << BatchNormalizationLayer(
           weights_accessor(total_path + "pointwise_BatchNorm_moving_mean.npy"),
           weights_accessor(total_path + "pointwise_BatchNorm_moving_variance.npy"),
           weights_accessor(total_path + "pointwise_BatchNorm_gamma.npy"),
           weights_accessor(total_path + "pointwise_BatchNorm_beta.npy"),
           0.001f)
       << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::BOUNDED_RELU, 6.f));

    return BranchLayer(std::move(sg));
}
} // namespace

void run_mobilenet()
{
    TargetHint            target_hint      = TargetHint::OPENCL;
    ConvolutionMethodHint convolution_hint = get_convolution_hint();
    
    TensorShape input_shape(session().image_size(),
                            session().image_size(),
                            3U,
                            session().batch_size());

    Graph graph;
    cout << "\nPrepare graph...\n";
    xopenme_clock_start(X_TIMER_SETUP);
    graph << target_hint
          << convolution_hint
          << Tensor(TensorInfo(input_shape, 1, DATATYPE), 
             arm_compute::support::cpp14::make_unique<CKNumPyInputLoader>())
          << ConvolutionLayer(
              3U, 3U, apply_multiplier(32U),
              weights_accessor("Conv2d_0_weights.npy"),
              empty_accessor(),
              PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR))
          << BatchNormalizationLayer(
              weights_accessor("Conv2d_0_BatchNorm_moving_mean.npy"),
              weights_accessor("Conv2d_0_BatchNorm_moving_variance.npy"),
              weights_accessor("Conv2d_0_BatchNorm_gamma.npy"),
              weights_accessor("Conv2d_0_BatchNorm_beta.npy"),
              0.001f)

          << ActivationLayer(ActivationLayerInfo(ActivationLayerInfo::ActivationFunction::BOUNDED_RELU, 6.f))
          << get_dwsc_node("Conv2d_1", 64, PadStrideInfo(1, 1, 1, 1), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_2", 128, PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_3", 128, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_4", 256, PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_5", 256, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_6", 512, PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_7", 512, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_8", 512, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_9", 512, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_10", 512, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_11", 512, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_12", 1024, PadStrideInfo(2, 2, 0, 1, 0, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << get_dwsc_node("Conv2d_13", 1024, PadStrideInfo(1, 1, 1, 1, 1, 1, DimensionRoundingType::FLOOR), PadStrideInfo(1, 1, 0, 0))
          << PoolingLayer(PoolingLayerInfo(PoolingType::AVG))
          << ConvolutionLayer(
              1U, 1U, 1001U,
              weights_accessor("Logits_Conv2d_1c_1x1_weights.npy"),
              weights_accessor("Logits_Conv2d_1c_1x1_biases.npy"),
              PadStrideInfo(1, 1, 0, 0))
          << ReshapeLayer(TensorShape(1001U))
          << SoftmaxLayer()
          << Tensor(arm_compute::support::cpp14::make_unique<CKOutputAccessor>());
    xopenme_clock_end(X_TIMER_SETUP);

    cout << "\nRun graph...\n";
    xopenme_clock_start(X_TIMER_TEST);
    graph.run();
    xopenme_clock_end(X_TIMER_TEST);
}
