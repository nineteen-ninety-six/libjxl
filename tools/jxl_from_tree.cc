// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <jxl/cms.h>
#include <jxl/memory_manager.h>
#include <jxl/types.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <istream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lib/extras/codec_in_out.h"
#include "lib/jxl/base/common.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/enc_cache.h"
#include "lib/jxl/enc_fields.h"
#include "lib/jxl/enc_frame.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/frame_dimensions.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_metadata.h"
#include "lib/jxl/modular/encoding/dec_ma.h"
#include "lib/jxl/modular/encoding/enc_debug_tree.h"
#include "lib/jxl/modular/options.h"
#include "lib/jxl/noise.h"
#include "lib/jxl/splines.h"
#include "lib/jxl/test_utils.h"  // TODO(eustas): cut this dependency
#include "tools/file_io.h"
#include "tools/no_memory_manager.h"

namespace jpegxl {
namespace tools {

using ::jxl::BitWriter;
using ::jxl::BlendMode;
using ::jxl::CodecInOut;
using ::jxl::CodecMetadata;
using ::jxl::ColorEncoding;
using ::jxl::ColorTransform;
using ::jxl::CompressParams;
using ::jxl::FrameDimensions;
using ::jxl::FrameInfo;
using ::jxl::Image3F;
using ::jxl::ImageF;
using ::jxl::PaddedBytes;
using ::jxl::PassesEncoderState;
using ::jxl::Predictor;
using ::jxl::PropertyDecisionNode;
using ::jxl::QuantizedSpline;
using ::jxl::Spline;
using ::jxl::Splines;
using ::jxl::StatusOr;
using ::jxl::Tree;

namespace {
struct SplineData {
  int32_t quantization_adjustment = 1;
  std::vector<Spline> splines;
};

StatusOr<Splines> SplinesFromSplineData(const SplineData& spline_data) {
  std::vector<QuantizedSpline> quantized_splines;
  std::vector<Spline::Point> starting_points;
  quantized_splines.reserve(spline_data.splines.size());
  starting_points.reserve(spline_data.splines.size());
  for (const Spline& spline : spline_data.splines) {
    JXL_ASSIGN_OR_RETURN(
        QuantizedSpline qspline,
        QuantizedSpline::Create(spline, spline_data.quantization_adjustment,
                                0.0, 1.0));
    quantized_splines.emplace_back(std::move(qspline));
    starting_points.push_back(spline.control_points.front());
  }
  return Splines(spline_data.quantization_adjustment,
                 std::move(quantized_splines), std::move(starting_points));
}

template <typename F>
bool ParseNode(F& tok, Tree& tree, SplineData& spline_data,
               CompressParams& cparams, size_t& W, size_t& H, CodecInOut& io,
               JXL_BOOL& have_next, int& x0, int& y0) {
  std::unordered_map<std::string, int> property_map = {
      {"c", 0},
      {"g", 1},
      {"y", 2},
      {"x", 3},
      {"|N|", 4},
      {"|W|", 5},
      {"N", 6},
      {"W", 7},
      {"W-WW-NW+NWW", 8},
      {"W+N-NW", 9},
      {"W-NW", 10},
      {"NW-N", 11},
      {"N-NE", 12},
      {"N-NN", 13},
      {"W-WW", 14},
      {"WGH", 15},
      {"PrevAbs", 16},
      {"Prev", 17},
      {"PrevAbsErr", 18},
      {"PrevErr", 19},
      {"PPrevAbs", 20},
      {"PPrev", 21},
      {"PPrevAbsErr", 22},
      {"PPrevErr", 23},
      {"Prev1Abs", 16},
      {"Prev1", 17},
      {"Prev1AbsErr", 18},
      {"Prev1Err", 19},
  };
  for (size_t i = 0; i < 19; i++) {
    std::string name_prefix = "Prev" + std::to_string(i + 1);
    property_map[name_prefix + "Abs"] = i * 4 + 16;
    property_map[name_prefix] = i * 4 + 17;
    property_map[name_prefix + "AbsErr"] = i * 4 + 18;
    property_map[name_prefix + "Err"] = i * 4 + 19;
  }
  static const std::unordered_map<std::string, Predictor> predictor_map = {
      {"Set", Predictor::Zero},
      {"W", Predictor::Left},
      {"N", Predictor::Top},
      {"AvgW+N", Predictor::Average0},
      {"Select", Predictor::Select},
      {"Gradient", Predictor::Gradient},
      {"Weighted", Predictor::Weighted},
      {"NE", Predictor::TopRight},
      {"NW", Predictor::TopLeft},
      {"WW", Predictor::LeftLeft},
      {"AvgW+NW", Predictor::Average1},
      {"AvgN+NW", Predictor::Average2},
      {"AvgN+NE", Predictor::Average3},
      {"AvgAll", Predictor::Average4},
  };
  auto t = tok();
  if (t == "if") {
    // Decision node.
    int p;
    t = tok();
    if (!property_map.count(t)) {
      fprintf(stderr, "Unexpected property: %s\n", t.c_str());
      return false;
    }
    p = property_map.at(t);
    t = tok();
    if (t != ">") {
      fprintf(stderr, "Expected >, found %s\n", t.c_str());
      return false;
    }
    t = tok();
    size_t num = 0;
    int split = std::stoi(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid splitval: %s\n", t.c_str());
      return false;
    }
    size_t pos = tree.size();
    tree.emplace_back(PropertyDecisionNode::Split(p, split, pos + 1));
    JXL_RETURN_IF_ERROR(ParseNode(tok, tree, spline_data, cparams, W, H, io,
                                  have_next, x0, y0));
    tree[pos].rchild = tree.size();
  } else if (t == "-") {
    // Leaf
    t = tok();
    Predictor p;
    if (!predictor_map.count(t)) {
      fprintf(stderr, "Unexpected predictor: %s\n", t.c_str());
      return false;
    }
    p = predictor_map.at(t);
    t = tok();
    bool subtract = false;
    if (t == "-") {
      subtract = true;
      t = tok();
    } else if (t == "+") {
      t = tok();
    }
    size_t num = 0;
    int offset = std::stoi(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid offset: %s\n", t.c_str());
      return false;
    }
    if (subtract) offset = -offset;
    tree.emplace_back(PropertyDecisionNode::Leaf(p, offset));
    return true;
  } else if (t == "Width") {
    t = tok();
    size_t num = 0;
    W = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid width: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Height") {
    t = tok();
    size_t num = 0;
    H = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid height: %s\n", t.c_str());
      return false;
    }
  } else if (t == "/*") {
    t = tok();
    while (t != "*/" && t != "") t = tok();
  } else if (t == "Squeeze") {
    cparams.responsive = true;
  } else if (t == "GroupShift") {
    t = tok();
    size_t num = 0;
    cparams.modular_group_size_shift = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid GroupShift: %s\n", t.c_str());
      return false;
    }
  } else if (t == "XYB") {
    cparams.color_transform = ColorTransform::kXYB;
  } else if (t == "CbYCr") {
    cparams.color_transform = ColorTransform::kYCbCr;
  } else if (t == "HiddenChannel") {
    t = tok();
    size_t num = 0;
    cparams.move_to_front_from_channel = -1 - std::stoul(t, &num);
    if (num != t.size() || num > 16) {
      fprintf(stderr, "Invalid HiddenChannel (max 16): %s\n", t.c_str());
      return false;
    }
  } else if (t == "RCT") {
    t = tok();
    size_t num = 0;
    cparams.colorspace = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid RCT: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Orientation") {
    t = tok();
    size_t num = 0;
    io.metadata.m.orientation = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid Orientation: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Alpha") {
    io.metadata.m.SetAlphaBits(io.metadata.m.bit_depth.bits_per_sample);
    JXL_ASSIGN_OR_RETURN(
        ImageF alpha, ImageF::Create(jpegxl::tools::NoMemoryManager(), W, H));
    if (!io.frames[0].SetAlpha(std::move(alpha))) {
      fprintf(stderr, "Internal: SetAlpha failed\n");
      return false;
    }
  } else if (t == "Bitdepth") {
    t = tok();
    size_t num = 0;
    uint32_t bits_per_sample = std::stoul(t, &num);
    if (num != t.size() || bits_per_sample < 1 || bits_per_sample > 32) {
      fprintf(stderr, "Invalid Bitdepth: %s\n", t.c_str());
      return false;
    }
    io.metadata.m.bit_depth.bits_per_sample = bits_per_sample;
  } else if (t == "FloatExpBits") {
    t = tok();
    size_t num = 0;
    io.metadata.m.bit_depth.floating_point_sample = true;
    io.metadata.m.bit_depth.exponent_bits_per_sample = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid FloatExpBits: %s\n", t.c_str());
      return false;
    }
  } else if (t == "FramePos") {
    t = tok();
    size_t num = 0;
    x0 = std::stoi(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid FramePos x0: %s\n", t.c_str());
      return false;
    }
    t = tok();
    y0 = std::stoi(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid FramePos y0: %s\n", t.c_str());
      return false;
    }
  } else if (t == "NotLast") {
    have_next = JXL_TRUE;
  } else if (t == "Upsample") {
    t = tok();
    size_t num = 0;
    cparams.resampling = std::stoul(t, &num);
    if (num != t.size() ||
        (cparams.resampling != 1 && cparams.resampling != 2 &&
         cparams.resampling != 4 && cparams.resampling != 8)) {
      fprintf(stderr, "Invalid Upsample: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Upsample_EC") {
    t = tok();
    size_t num = 0;
    cparams.ec_resampling = std::stoul(t, &num);
    if (num != t.size() ||
        (cparams.ec_resampling != 1 && cparams.ec_resampling != 2 &&
         cparams.ec_resampling != 4 && cparams.ec_resampling != 8)) {
      fprintf(stderr, "Invalid Upsample_EC: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Animation") {
    io.metadata.m.have_animation = true;
    io.metadata.m.animation.tps_numerator = 1000;
    io.metadata.m.animation.tps_denominator = 1;
    io.frames[0].duration = 100;
  } else if (t == "AnimationFPS") {
    t = tok();
    size_t num = 0;
    io.metadata.m.animation.tps_numerator = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid numerator: %s\n", t.c_str());
      return false;
    }
    t = tok();
    num = 0;
    io.metadata.m.animation.tps_denominator = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid denominator: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Duration") {
    t = tok();
    size_t num = 0;
    io.frames[0].duration = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid Duration: %s\n", t.c_str());
      return false;
    }
  } else if (t == "BlendMode") {
    t = tok();
    if (t == "kAdd") {
      io.frames[0].blendmode = BlendMode::kAdd;
    } else if (t == "kReplace") {
      io.frames[0].blendmode = BlendMode::kReplace;
    } else if (t == "kBlend") {
      io.frames[0].blendmode = BlendMode::kBlend;
    } else if (t == "kAlphaWeightedAdd") {
      io.frames[0].blendmode = BlendMode::kAlphaWeightedAdd;
    } else if (t == "kMul") {
      io.frames[0].blendmode = BlendMode::kMul;
    } else {
      fprintf(stderr, "Invalid BlendMode: %s\n", t.c_str());
      return false;
    }
  } else if (t == "SplineQuantizationAdjustment") {
    t = tok();
    size_t num = 0;
    spline_data.quantization_adjustment = std::stoul(t, &num);
    if (num != t.size()) {
      fprintf(stderr, "Invalid SplineQuantizationAdjustment: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Spline") {
    Spline spline;
    const auto ParseFloat = [&t, &tok](float& output) {
      t = tok();
      size_t num = 0;
      output = std::stof(t, &num);
      if (num != t.size()) {
        fprintf(stderr, "Invalid spline data: %s\n", t.c_str());
        return false;
      }
      return true;
    };
    for (auto& dct : spline.color_dct) {
      for (float& coefficient : dct) {
        JXL_RETURN_IF_ERROR(ParseFloat(coefficient));
      }
    }
    for (float& coefficient : spline.sigma_dct) {
      JXL_RETURN_IF_ERROR(ParseFloat(coefficient));
    }

    while (true) {
      t = tok();
      if (t == "EndSpline") break;
      size_t num = 0;
      Spline::Point point;
      point.x = std::stof(t, &num);
      bool ok_x = num == t.size();
      auto t_y = tok();
      point.y = std::stof(t_y, &num);
      if (!ok_x || num != t_y.size()) {
        fprintf(stderr, "Invalid spline control point: %s %s\n", t.c_str(),
                t_y.c_str());
        return false;
      }
      spline.control_points.push_back(point);
    }

    if (spline.control_points.empty()) {
      fprintf(stderr, "Spline with no control point\n");
      return false;
    }

    spline_data.splines.push_back(std::move(spline));
  } else if (t == "Gaborish") {
    cparams.gaborish = jxl::Override::kOn;
  } else if (t == "DeltaPalette") {
    cparams.lossy_palette = true;
    cparams.palette_colors = 0;
  } else if (t == "EPF") {
    t = tok();
    size_t num = 0;
    cparams.epf = std::stoul(t, &num);
    if (num != t.size() || cparams.epf > 3) {
      fprintf(stderr, "Invalid EPF: %s\n", t.c_str());
      return false;
    }
  } else if (t == "Noise") {
    cparams.manual_noise.resize(8);
    for (size_t i = 0; i < 8; i++) {
      t = tok();
      size_t num = 0;
      float v = std::stof(t, &num);
      if (num != t.size() || v < 0.0f || v > 1.0f) {
        fprintf(stderr, "Invalid noise entry: %s\n", t.c_str());
        return false;
      }
      cparams.manual_noise[i] = jxl::Clamp1(v, 0.0f, jxl::kNoiseLutMax);
    }
  } else if (t == "XYBFactors") {
    cparams.manual_xyb_factors.resize(3);
    for (size_t i = 0; i < 3; i++) {
      t = tok();
      size_t num = 0;
      cparams.manual_xyb_factors[i] = std::stof(t, &num);
      if (num != t.size()) {
        fprintf(stderr, "Invalid XYB factor: %s\n", t.c_str());
        return false;
      }
    }
  } else if (t == "PQ") {
    io.metadata.m.color_encoding.Tf().transfer_function =
        jxl::TransferFunction::kPQ;
    io.metadata.m.tone_mapping.intensity_target = 10000;
  } else if (t == "HLG") {
    io.metadata.m.color_encoding.Tf().transfer_function =
        jxl::TransferFunction::kHLG;
    io.metadata.m.tone_mapping.intensity_target = 1000;
  } else if (t == "Rec2100") {
    JXL_RETURN_IF_ERROR(
        io.metadata.m.color_encoding.SetPrimariesType(jxl::Primaries::k2100));
  } else if (t == "P3") {
    JXL_RETURN_IF_ERROR(
        io.metadata.m.color_encoding.SetPrimariesType(jxl::Primaries::kP3));
  } else if (t == "16BitBuffers") {
    io.metadata.m.modular_16_bit_buffer_sufficient = true;
  } else {
    fprintf(stderr, "Unexpected node type: %s\n", t.c_str());
    return false;
  }
  JXL_RETURN_IF_ERROR(
      ParseNode(tok, tree, spline_data, cparams, W, H, io, have_next, x0, y0));
  return true;
}
}  // namespace

::jxl::Status JxlFromTree(const char* in, const char* out,
                          const char* tree_out) {
  Tree tree;
  SplineData spline_data;
  CompressParams cparams = {};
  size_t width = 1024;
  size_t height = 1024;
  int x0 = 0;
  int y0 = 0;
  cparams.SetLossless();
  cparams.responsive = JXL_FALSE;
  cparams.resampling = 1;
  cparams.ec_resampling = 1;
  cparams.modular_group_size_shift = 3;
  cparams.colorspace = 0;
  cparams.buffering = 0;
  JxlMemoryManager* memory_manager = jpegxl::tools::NoMemoryManager();
  auto io = jxl::make_unique<CodecInOut>(memory_manager);
  io->metadata.m.modular_16_bit_buffer_sufficient = false;
  int have_next = JXL_FALSE;

  std::istream* f = &std::cin;
  std::ifstream file;

  if (strcmp(in, "-") > 0) {
    file.open(in, std::ifstream::in);
    f = &file;
  }

  auto tok = [&f]() {
    std::string out;
    *f >> out;
    return out;
  };
  if (!ParseNode(tok, tree, spline_data, cparams, width, height, *io, have_next,
                 x0, y0)) {
    return JXL_FAILURE("Failed to ParseNode");
  }

  if (tree_out) {
    PrintTree(tree, tree_out);
  }
  JXL_ASSIGN_OR_RETURN(
      Image3F image, Image3F::Create(memory_manager, width * cparams.resampling,
                                     height * cparams.resampling));
  JXL_RETURN_IF_ERROR(
      io->SetFromImage(std::move(image), io->metadata.m.color_encoding));
  JXL_RETURN_IF_ERROR(io->SetSize((width + x0) * cparams.resampling,
                                  (height + y0) * cparams.resampling));

  io->metadata.m.color_encoding.DecideIfWantICC(*JxlGetDefaultCms());
  cparams.options.zero_tokens = true;
  cparams.palette_colors = 0;
  cparams.channel_colors_pre_transform_percent = 0;
  cparams.channel_colors_percent = 0;
  cparams.patches = jxl::Override::kOff;
  cparams.already_downsampled = true;
  cparams.custom_fixed_tree = tree;
  JXL_ASSIGN_OR_RETURN(cparams.custom_splines,
                       SplinesFromSplineData(spline_data));
  PaddedBytes compressed{memory_manager};

  JXL_RETURN_IF_ERROR(io->CheckMetadata());
  BitWriter writer{memory_manager};

  std::unique_ptr<CodecMetadata> metadata = jxl::make_unique<CodecMetadata>();
  *metadata = io->metadata;
  JXL_RETURN_IF_ERROR(metadata->size.Set(io->xsize(), io->ysize()));

  metadata->m.xyb_encoded = (cparams.color_transform == ColorTransform::kXYB);

  if (cparams.move_to_front_from_channel < -1) {
    size_t nch = -1 - cparams.move_to_front_from_channel;
    cparams.move_to_front_from_channel = 3 + metadata->m.num_extra_channels;
    metadata->m.num_extra_channels += nch;
    for (size_t _ = 0; _ < nch; _++) {
      metadata->m.extra_channel_info.emplace_back();
      auto& eci = metadata->m.extra_channel_info.back();
      eci.type = jxl::ExtraChannel::kOptional;
      JXL_ASSIGN_OR_RETURN(
          ImageF ch, ImageF::Create(memory_manager, io->xsize(), io->ysize()));
      io->frames[0].extra_channels().emplace_back(std::move(ch));
    }
  }

  JXL_RETURN_IF_ERROR(WriteCodestreamHeaders(metadata.get(), &writer, nullptr));
  writer.ZeroPadToByte();

  while (true) {
    FrameInfo info;
    info.is_last = !FROM_JXL_BOOL(have_next);
    if (!info.is_last) info.save_as_reference = 1;

    io->frames[0].origin.x0 = x0;
    io->frames[0].origin.y0 = y0;
    info.clamp = false;

    JXL_RETURN_IF_ERROR(jxl::EncodeFrame(
        memory_manager, cparams, info, metadata.get(), io->frames[0],
        *JxlGetDefaultCms(), nullptr, &writer, nullptr));
    if (!have_next) break;
    tree.clear();
    spline_data.splines.clear();
    have_next = JXL_FALSE;
    cparams.manual_noise.clear();
    if (!ParseNode(tok, tree, spline_data, cparams, width, height, *io,
                   have_next, x0, y0)) {
      return JXL_FAILURE("Failed to ParseNode");
    }
    cparams.custom_fixed_tree = tree;
    JXL_ASSIGN_OR_RETURN(Image3F image,
                         Image3F::Create(memory_manager, width, height));
    JXL_RETURN_IF_ERROR(
        io->SetFromImage(std::move(image), ColorEncoding::SRGB()));
    io->frames[0].blend = true;
  }

  compressed = std::move(writer).TakeBytes();

  if (!WriteFile(out, compressed)) {
    fprintf(stderr, "Failed to write to \"%s\"\n", out);
    return JXL_FAILURE("Failed to write output");
  }

  return true;
}
}  // namespace tools
}  // namespace jpegxl

int main(int argc, char** argv) {
  if ((argc != 3 && argc != 4) ||
      ((strcmp(argv[1], "-") > 0) && !strcmp(argv[1], argv[2]))) {
    fprintf(stderr, "Usage: %s tree_in.txt out.jxl [tree_drawing]\n", argv[0]);
    return 1;
  }
  jxl::Status result = jpegxl::tools::JxlFromTree(argv[1], argv[2],
                                                  argc < 4 ? nullptr : argv[3]);
  if (!result) {
    fprintf(stderr, "FAILURE\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
