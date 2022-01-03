//
// Implementation for Yocto/Scene Input and Output functions.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2021 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "yocto_sceneio.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#define JSON_USE_IMPLICIT_CONVERSIONS 0
#include "ext/json.hpp"
#include "ext/stb_image.h"
#include "ext/stb_image_resize.h"
#include "ext/stb_image_write.h"
#include "ext/tinyexr.h"
#include "yocto_color.h"
#include "yocto_geometry.h"
#include "yocto_image.h"
#include "yocto_modelio.h"
#include "yocto_shape.h"

// -----------------------------------------------------------------------------
// USING DIRECTIVES
// -----------------------------------------------------------------------------
namespace yocto {

  // using directives
  using std::unique_ptr;
  using namespace std::string_literals;

}  // namespace yocto

// -----------------------------------------------------------------------------
// PARALLEL HELPERS
// -----------------------------------------------------------------------------
namespace yocto {

  // Simple parallel for used since our target platforms do not yet support
  // parallel algorithms. `Func` takes the integer index.
  template <typename T, typename Func>
  inline bool parallel_for(T num, string& error, Func&& func) {
    auto              futures  = vector<std::future<void>>{};
    auto              nthreads = std::thread::hardware_concurrency();
    std::atomic<T>    next_idx(0);
    std::atomic<bool> has_error(false);
    std::mutex        error_mutex;
    for (auto thread_id = 0; thread_id < (int)nthreads; thread_id++) {
      futures.emplace_back(std::async(std::launch::async,
          [&func, &next_idx, &has_error, &error_mutex, &error, num]() {
            auto this_error = string{};
            while (true) {
              if (has_error) break;
              auto idx = next_idx.fetch_add(1);
              if (idx >= num) break;
              if (!func(idx, this_error)) {
                has_error = true;
                auto _    = std::lock_guard{error_mutex};
                error     = this_error;
                break;
              }
            }
          }));
    }
    for (auto& f : futures) f.get();
    return !(bool)has_error;
  }

  // Simple parallel for used since our target platforms do not yet support
  // parallel algorithms. `Func` takes a reference to a `T`.
  template <typename T, typename Func>
  inline bool parallel_foreach(vector<T>& values, string& error, Func&& func) {
    return parallel_for(
        values.size(), error, [&func, &values](size_t idx, string& error) {
          return func(values[idx], error);
        });
  }
  template <typename T, typename Func>
  inline bool parallel_foreach(
      const vector<T>& values, string& error, Func&& func) {
    return parallel_for(
        values.size(), error, [&func, &values](size_t idx, string& error) {
          return func(values[idx], error);
        });
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// PATH UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

  // Make a path from a utf8 string
  static std::filesystem::path make_path(const string& filename) {
    return std::filesystem::u8path(filename);
  }

  // Get directory name (not including /)
  static string path_dirname(const string& filename) {
    return make_path(filename).parent_path().generic_u8string();
  }

  // Get extension (including .)
  static string path_extension(const string& filename) {
    return make_path(filename).extension().u8string();
  }

  // Get filename without directory and extension.
  static string path_basename(const string& filename) {
    return make_path(filename).stem().u8string();
  }

  // Joins paths
  static string path_join(const string& patha, const string& pathb) {
    return (make_path(patha) / make_path(pathb)).generic_u8string();
  }

  // Check if a file can be opened for reading.
  static bool path_exists(const string& filename) {
    return exists(make_path(filename));
  }

  // Create a directory and all missing parent directories if needed
  static bool make_directory(const string& dirname, string& error) {
    if (path_exists(dirname)) return true;
    try {
      create_directories(make_path(dirname));
      return true;
    } catch (...) {
      error = dirname + ": cannot create directory";
      return false;
    }
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// FILE IO
// -----------------------------------------------------------------------------
namespace yocto {

  // Opens a file with a utf8 file name
  static FILE* fopen_utf8(const char* filename, const char* mode) {
#ifdef _WIN32
    auto path8    = std::filesystem::u8path(filename);
    auto str_mode = string{mode};
    auto wmode    = std::wstring(str_mode.begin(), str_mode.end());
    return _wfopen(path8.c_str(), wmode.c_str());
#else
    return fopen(filename, mode);
#endif
  }

  // Opens a file with utf8 filename
  FILE* fopen_utf8(const string& filename, const string& mode) {
#ifdef _WIN32
    auto path8 = std::filesystem::u8path(filename);
    auto wmode = std::wstring(mode.begin(), mode.end());
    return _wfopen(path8.c_str(), wmode.c_str());
#else
    return fopen(filename.c_str(), mode.c_str());
#endif
  }

  // Load a text file
  string load_text(const string& filename) {
    auto error = string{};
    auto str   = string{};
    if (!load_text(filename, str, error)) throw io_error{error};
    return str;
  }
  void load_text(const string& filename, string& text) {
    auto error = string{};
    if (!load_text(filename, text, error)) throw io_error{error};
  }

  // Save a text file
  void save_text(const string& filename, const string& text) {
    auto error = string{};
    if (!save_text(filename, text, error)) throw io_error{error};
  }

  // Load a binary file
  vector<byte> load_binary(const string& filename) {
    auto error = string{};
    auto data  = vector<byte>{};
    if (!load_binary(filename, data, error)) throw io_error{error};
    return data;
  }
  void load_binary(const string& filename, vector<byte>& data) {
    auto error = string{};
    if (!load_binary(filename, data, error)) throw io_error{error};
  }

  // Save a binary file
  void save_binary(const string& filename, const vector<byte>& data) {
    auto error = string{};
    if (!save_binary(filename, data, error)) throw io_error{error};
  }

  // Load a text file
  bool load_text(const string& filename, string& str, string& error) {
    // https://stackoverflow.com/questions/174531/how-to-read-the-content-of-a-file-to-a-string-in-c
    auto fs = fopen_utf8(filename.c_str(), "rb");
    if (!fs) {
      error = "cannot open " + filename;
      return false;
    }
    fseek(fs, 0, SEEK_END);
    auto length = ftell(fs);
    fseek(fs, 0, SEEK_SET);
    str.resize(length);
    if (fread(str.data(), 1, length, fs) != length) {
      fclose(fs);
      error = "cannot read " + filename;
      return false;
    }
    fclose(fs);
    return true;
  }

  // Save a text file
  bool save_text(const string& filename, const string& str, string& error) {
    auto fs = fopen_utf8(filename.c_str(), "wt");
    if (!fs) {
      error = "cannot create " + filename;
      return false;
    }
    if (fprintf(fs, "%s", str.c_str()) < 0) {
      fclose(fs);
      error = "cannot write " + filename;
      return false;
    }
    fclose(fs);
    return true;
  }

  // Load a binary file
  bool load_binary(const string& filename, vector<byte>& data, string& error) {
    // https://stackoverflow.com/questions/174531/how-to-read-the-content-of-a-file-to-a-string-in-c
    auto fs = fopen_utf8(filename.c_str(), "rb");
    if (!fs) {
      error = "cannot open " + filename;
      return false;
    }
    fseek(fs, 0, SEEK_END);
    auto length = ftell(fs);
    fseek(fs, 0, SEEK_SET);
    data.resize(length);
    if (fread(data.data(), 1, length, fs) != length) {
      fclose(fs);
      error = "cannot read " + filename;
      return false;
    }
    fclose(fs);
    return true;
  }

  // Save a binary file
  bool save_binary(
      const string& filename, const vector<byte>& data, string& error) {
    auto fs = fopen_utf8(filename.c_str(), "wb");
    if (!fs) {
      error = "cannot create " + filename;
      return false;
    }
    if (fwrite(data.data(), 1, data.size(), fs) != data.size()) {
      fclose(fs);
      error = "cannot write " + filename;
      return false;
    }
    fclose(fs);
    return true;
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// JSON SUPPORT
// -----------------------------------------------------------------------------
namespace yocto {

  // Json values
  using json_value = nlohmann::ordered_json;

  // Load/save json
  static bool load_json(
      const string& filename, json_value& json, string& error) {
    auto text = string{};
    if (!load_text(filename, text, error)) return false;
    try {
      json = json_value::parse(text);
      return true;
    } catch (...) {
      error = "cannot parse " + filename;
      return false;
    }
  }
  static bool save_json(
      const string& filename, const json_value& json, string& error) {
    return save_text(filename, json.dump(2), error);
  }

  // Load/save json
  [[maybe_unused]] static json_value load_json(const string& filename) {
    auto error = string{};
    auto json  = json_value{};
    if (!load_json(filename, json, error)) throw io_error{error};
    return json;
  }
  [[maybe_unused]] static void load_json(
      const string& filename, json_value& json) {
    auto error = string{};
    if (!load_json(filename, json, error)) throw io_error{error};
  }
  [[maybe_unused]] static void save_json(
      const string& filename, const json_value& json) {
    auto error = string{};
    if (!save_json(filename, json, error)) throw io_error{error};
  }

  // Json conversions
  inline void to_json(json_value& json, const vec2f& value) {
    nlohmann::to_json(json, (const array<float, 2>&)value);
  }
  inline void to_json(json_value& json, const vec3f& value) {
    nlohmann::to_json(json, (const array<float, 3>&)value);
  }
  inline void to_json(json_value& json, const vec4f& value) {
    nlohmann::to_json(json, (const array<float, 4>&)value);
  }
  inline void to_json(json_value& json, const frame2f& value) {
    nlohmann::to_json(json, (const array<float, 6>&)value);
  }
  inline void to_json(json_value& json, const frame3f& value) {
    nlohmann::to_json(json, (const array<float, 12>&)value);
  }
  inline void to_json(json_value& json, const mat2f& value) {
    nlohmann::to_json(json, (const array<float, 4>&)value);
  }
  inline void to_json(json_value& json, const mat3f& value) {
    nlohmann::to_json(json, (const array<float, 9>&)value);
  }
  inline void to_json(json_value& json, const mat4f& value) {
    nlohmann::to_json(json, (const array<float, 16>&)value);
  }
  inline void from_json(const json_value& json, vec2f& value) {
    nlohmann::from_json(json, (array<float, 2>&)value);
  }
  inline void from_json(const json_value& json, vec3f& value) {
    nlohmann::from_json(json, (array<float, 3>&)value);
  }
  inline void from_json(const json_value& json, vec4f& value) {
    nlohmann::from_json(json, (array<float, 4>&)value);
  }
  inline void from_json(const json_value& json, frame2f& value) {
    nlohmann::from_json(json, (array<float, 6>&)value);
  }
  inline void from_json(const json_value& json, frame3f& value) {
    nlohmann::from_json(json, (array<float, 12>&)value);
  }
  inline void from_json(const json_value& json, mat2f& value) {
    nlohmann::from_json(json, (array<float, 4>&)value);
  }
  inline void from_json(const json_value& json, mat3f& value) {
    nlohmann::from_json(json, (array<float, 9>&)value);
  }
  inline void from_json(const json_value& json, mat4f& value) {
    nlohmann::from_json(json, (array<float, 16>&)value);
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// IMAGE IO
// -----------------------------------------------------------------------------
namespace yocto {

  // Check if an image is HDR based on filename.
  bool is_hdr_filename(const string& filename) {
    auto ext = path_extension(filename);
    return ext == ".hdr" || ext == ".exr" || ext == ".pfm";
  }

  bool is_ldr_filename(const string& filename) {
    auto ext = path_extension(filename);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".tga";
  }

  // Loads/saves an image. Chooses hdr or ldr based on file name.
  bool load_image(const string& filename, image_data& image, string& error) {
    auto read_error = [&]() {
      error = "cannot read " + filename;
      return false;
    };

    // conversion helpers
    auto from_linear = [](const float* pixels, int width, int height) {
      return vector<vec4f>{
          (vec4f*)pixels, (vec4f*)pixels + (size_t)width * (size_t)height};
    };
    auto from_srgb = [](const byte* pixels, int width, int height) {
      auto pixelsf = vector<vec4f>((size_t)width * (size_t)height);
      for (auto idx = (size_t)0; idx < pixelsf.size(); idx++) {
        pixelsf[idx] = byte_to_float(((vec4b*)pixels)[idx]);
      }
      return pixelsf;
    };

    auto ext = path_extension(filename);
    if (ext == ".exr" || ext == ".EXR") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto pixels = (float*)nullptr;
      if (LoadEXRFromMemory(&pixels, &image.width, &image.height, buffer.data(),
              buffer.size(), nullptr) != 0)
        return read_error();
      image.linear = true;
      image.pixels = from_linear(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".hdr" || ext == ".HDR") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_loadf_from_memory(buffer.data(), (int)buffer.size(),
          &image.width, &image.height, &ncomp, 4);
      if (!pixels) return read_error();
      image.linear = true;
      image.pixels = from_linear(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".png" || ext == ".PNG") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &image.width, &image.height, &ncomp, 4);
      if (!pixels) return read_error();
      image.linear = false;
      image.pixels = from_srgb(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" ||
               ext == ".JPEG") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &image.width, &image.height, &ncomp, 4);
      if (!pixels) return read_error();
      image.linear = false;
      image.pixels = from_srgb(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".tga" || ext == ".TGA") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &image.width, &image.height, &ncomp, 4);
      if (!pixels) return read_error();
      image.linear = false;
      image.pixels = from_srgb(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".bmp" || ext == ".BMP") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &image.width, &image.height, &ncomp, 4);
      if (!pixels) return read_error();
      image.linear = false;
      image.pixels = from_srgb(pixels, image.width, image.height);
      free(pixels);
      return true;
    } else if (ext == ".ypreset" || ext == ".YPRESET") {
      // create preset
      if (!make_image_preset(filename, image, error)) return false;
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Saves an hdr image.
  bool save_image(
      const string& filename, const image_data& image, string& error) {
    auto write_error = [&]() {
      error = "cannot write " + filename;
      return false;
    };

    // conversion helpers
    auto to_linear = [](const image_data& image) {
      if (image.linear) return image.pixels;
      auto pixelsf = vector<vec4f>(image.pixels.size());
      srgb_to_rgb(pixelsf, image.pixels);
      return pixelsf;
    };
    auto to_srgb = [](const image_data& image) {
      auto pixelsb = vector<vec4b>(image.pixels.size());
      if (image.linear) {
        rgb_to_srgb(pixelsb, image.pixels);
      } else {
        float_to_byte(pixelsb, image.pixels);
      }
      return pixelsb;
    };

    // write data
    auto stbi_write_data = [](void* context, void* data, int size) {
      auto& buffer = *(vector<byte>*)context;
      buffer.insert(buffer.end(), (byte*)data, (byte*)data + size);
    };

    auto ext = path_extension(filename);
    if (ext == ".hdr" || ext == ".HDR") {
      auto buffer = vector<byte>{};
      if (!stbi_write_hdr_to_func(stbi_write_data, &buffer, (int)image.width,
              (int)image.height, 4, (const float*)to_linear(image).data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".exr" || ext == ".EXR") {
      auto data = (byte*)nullptr;
      auto size = (size_t)0;
      if (SaveEXRToMemory((const float*)to_linear(image).data(),
              (int)image.width, (int)image.height, 4, 1, &data, &size,
              nullptr) < 0)
        return write_error();
      auto buffer = vector<byte>{data, data + size};
      free(data);
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".png" || ext == ".PNG") {
      auto buffer = vector<byte>{};
      if (!stbi_write_png_to_func(stbi_write_data, &buffer, (int)image.width,
              (int)image.height, 4, (const byte*)to_srgb(image).data(),
              (int)image.width * 4))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" ||
               ext == ".JPEG") {
      auto buffer = vector<byte>{};
      if (!stbi_write_jpg_to_func(stbi_write_data, &buffer, (int)image.width,
              (int)image.height, 4, (const byte*)to_srgb(image).data(), 75))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".tga" || ext == ".TGA") {
      auto buffer = vector<byte>{};
      if (!stbi_write_tga_to_func(stbi_write_data, &buffer, (int)image.width,
              (int)image.height, 4, (const byte*)to_srgb(image).data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".bmp" || ext == ".BMP") {
      auto buffer = vector<byte>{};
      if (!stbi_write_bmp_to_func(stbi_write_data, &buffer, (int)image.width,
              (int)image.height, 4, (const byte*)to_srgb(image).data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  image_data make_image_preset(const string& type_) {
    auto type  = path_basename(type_);
    auto width = 1024, height = 1024;
    if (type.find("sky") != type.npos) width = 2048;
    if (type.find("images2") != type.npos) width = 2048;
    if (type == "grid") {
      return make_grid(width, height);
    } else if (type == "checker") {
      return make_checker(width, height);
    } else if (type == "bumps") {
      return make_bumps(width, height);
    } else if (type == "uvramp") {
      return make_uvramp(width, height);
    } else if (type == "gammaramp") {
      return make_gammaramp(width, height);
    } else if (type == "blackbodyramp") {
      return make_blackbodyramp(width, height);
    } else if (type == "uvgrid") {
      return make_uvgrid(width, height);
    } else if (type == "colormapramp") {
      return make_colormapramp(width, height);
    } else if (type == "sky") {
      return make_sunsky(width, height, pif / 4, 3.0f, false, 1.0f, 1.0f,
          vec3f{0.7f, 0.7f, 0.7f});
    } else if (type == "sunsky") {
      return make_sunsky(width, height, pif / 4, 3.0f, true, 1.0f, 1.0f,
          vec3f{0.7f, 0.7f, 0.7f});
    } else if (type == "noise") {
      return make_noisemap(width, height, 1);
    } else if (type == "fbm") {
      return make_fbmmap(width, height, 1);
    } else if (type == "ridge") {
      return make_ridgemap(width, height, 1);
    } else if (type == "turbulence") {
      return make_turbulencemap(width, height, 1);
    } else if (type == "bump-normal") {
      return make_bumps(width, height);
      // TODO(fabio): fix color space
      // img   = srgb_to_rgb(bump_to_normal(img, 0.05f));
    } else if (type == "images1") {
      auto sub_types  = vector<string>{"grid", "uvgrid", "checker", "gammaramp",
          "bumps", "bump-normal", "noise", "fbm", "blackbodyramp"};
      auto sub_images = vector<image_data>();
      for (auto& sub_type : sub_types)
        sub_images.push_back(make_image_preset(sub_type));
      auto montage_size = zero2i;
      for (auto& sub_image : sub_images) {
        montage_size.x += sub_image.width;
        montage_size.y = max(montage_size.y, sub_image.height);
      }
      auto image = make_image(
          montage_size.x, montage_size.y, sub_images[0].linear);
      auto pos = 0;
      for (auto& sub_image : sub_images) {
        set_region(image, sub_image, pos, 0);
        pos += sub_image.width;
      }
      return image;
    } else if (type == "images2") {
      auto sub_types  = vector<string>{"sky", "sunsky"};
      auto sub_images = vector<image_data>();
      for (auto& sub_type : sub_types)
        sub_images.push_back(make_image_preset(sub_type));
      auto montage_size = zero2i;
      for (auto& sub_image : sub_images) {
        montage_size.x += sub_image.width;
        montage_size.y = max(montage_size.y, sub_image.height);
      }
      auto image = make_image(
          montage_size.x, montage_size.y, sub_images[0].linear);
      auto pos = 0;
      for (auto& sub_image : sub_images) {
        set_region(image, sub_image, pos, 0);
        pos += sub_image.width;
      }
      return image;
    } else if (type == "test-floor") {
      return add_border(make_grid(width, height), 0.0025f);
    } else if (type == "test-grid") {
      return make_grid(width, height);
    } else if (type == "test-checker") {
      return make_checker(width, height);
    } else if (type == "test-bumps") {
      return make_bumps(width, height);
    } else if (type == "test-uvramp") {
      return make_uvramp(width, height);
    } else if (type == "test-gammaramp") {
      return make_gammaramp(width, height);
    } else if (type == "test-blackbodyramp") {
      return make_blackbodyramp(width, height);
    } else if (type == "test-colormapramp") {
      return make_colormapramp(width, height);
      // TODO(fabio): fix color space
      // img   = srgb_to_rgb(img);
    } else if (type == "test-uvgrid") {
      return make_uvgrid(width, height);
    } else if (type == "test-sky") {
      return make_sunsky(width, height, pif / 4, 3.0f, false, 1.0f, 1.0f,
          vec3f{0.7f, 0.7f, 0.7f});
    } else if (type == "test-sunsky") {
      return make_sunsky(width, height, pif / 4, 3.0f, true, 1.0f, 1.0f,
          vec3f{0.7f, 0.7f, 0.7f});
    } else if (type == "test-noise") {
      return make_noisemap(width, height);
    } else if (type == "test-fbm") {
      return make_noisemap(width, height);
    } else if (type == "test-bumps-normal") {
      return bump_to_normal(make_bumps(width, height), 0.05f);
    } else if (type == "test-bumps-displacement") {
      return make_bumps(width, height);
      // TODO(fabio): fix color space
      // img   = srgb_to_rgb(img);
    } else if (type == "test-fbm-displacement") {
      return make_fbmmap(width, height);
      // TODO(fabio): fix color space
      // img   = srgb_to_rgb(img);
    } else if (type == "test-checker-opacity") {
      return make_checker(width, height, 1, {1, 1, 1, 1}, {0, 0, 0, 0});
    } else if (type == "test-grid-opacity") {
      return make_grid(width, height, 1, {1, 1, 1, 1}, {0, 0, 0, 0});
    } else {
      return {};
    }
  }

  // Loads/saves an image. Chooses hdr or ldr based on file name.
  image_data load_image(const string& filename, string& error) {
    auto image = image_data{};
    if (!load_image(filename, image, error)) return image_data{};
    return image;
  }
  image_data load_image(const string& filename) {
    auto error = string{};
    auto image = image_data{};
    if (!load_image(filename, image, error)) throw io_error{error};
    return image;
  }
  void load_image(const string& filename, image_data& image) {
    auto error = string{};
    if (!load_image(filename, image, error)) throw io_error{error};
  }
  void save_image(const string& filename, const image_data& image) {
    auto error = string{};
    if (!save_image(filename, image, error)) throw io_error{error};
  }

  bool make_image_preset(
      const string& filename, image_data& image, string& error) {
    image = make_image_preset(path_basename(filename));
    if (image.pixels.empty()) {
      error = "unknown preset";
      return false;
    }
    return true;
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// SHAPE IO
// -----------------------------------------------------------------------------
namespace yocto {

  // Load mesh
  bool load_shape(const string& filename, shape_data& shape, string& error,
      bool flip_texcoord) {
    auto shape_error = [&]() {
      error = "empty shape " + filename;
      return false;
    };

    shape = {};

    auto ext = path_extension(filename);
    if (ext == ".ply" || ext == ".PLY") {
      auto ply = ply_model{};
      if (!load_ply(filename, ply, error)) return false;
      // TODO: remove when all as arrays
      get_positions(ply, (vector<array<float, 3>>&)shape.positions);
      get_normals(ply, (vector<array<float, 3>>&)shape.normals);
      get_texcoords(
          ply, (vector<array<float, 2>>&)shape.texcoords, flip_texcoord);
      get_colors(ply, (vector<array<float, 4>>&)shape.colors);
      get_radius(ply, shape.radius);
      get_faces(ply, (vector<array<int, 3>>&)shape.triangles,
          (vector<array<int, 4>>&)shape.quads);
      get_lines(ply, (vector<array<int, 2>>&)shape.lines);
      get_points(ply, shape.points);
      if (shape.points.empty() && shape.lines.empty() &&
          shape.triangles.empty() && shape.quads.empty())
        return shape_error();
      return true;
    } else if (ext == ".obj" || ext == ".OBJ") {
      auto obj = obj_shape{};
      if (!load_obj(filename, obj, error, false)) return false;
      auto materials = vector<int>{};
      // TODO: remove when all as arrays
      get_positions(obj, (vector<array<float, 3>>&)shape.positions);
      get_normals(obj, (vector<array<float, 3>>&)shape.normals);
      get_texcoords(
          obj, (vector<array<float, 2>>&)shape.texcoords, flip_texcoord);
      get_faces(obj, (vector<array<int, 3>>&)shape.triangles,
          (vector<array<int, 4>>&)shape.quads, materials);
      get_lines(obj, (vector<array<int, 2>>&)shape.lines, materials);
      get_points(obj, shape.points, materials);
      if (shape.points.empty() && shape.lines.empty() &&
          shape.triangles.empty() && shape.quads.empty())
        return shape_error();
      return true;
    } else if (ext == ".stl" || ext == ".STL") {
      auto stl = stl_model{};
      if (!load_stl(filename, stl, error, true)) return false;
      if (stl.shapes.size() != 1) return shape_error();
      auto fnormals = vector<vec3f>{};
      if (!get_triangles(stl, 0, (vector<array<int, 3>>&)shape.triangles,
              (vector<array<float, 3>>&)shape.positions,
              (vector<array<float, 3>>&)fnormals))
        return shape_error();
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Save ply mesh
  bool save_shape(const string& filename, const shape_data& shape,
      string& error, bool flip_texcoord, bool ascii) {
    auto shape_error = [&]() {
      error = "empty shape " + filename;
      return false;
    };

    auto ext = path_extension(filename);
    if (ext == ".ply" || ext == ".PLY") {
      auto ply = ply_model{};
      // TODO: remove when all as arrays
      add_positions(ply, (const vector<array<float, 3>>&)shape.positions);
      add_normals(ply, (const vector<array<float, 3>>&)shape.normals);
      add_texcoords(
          ply, (const vector<array<float, 2>>&)shape.texcoords, flip_texcoord);
      add_colors(ply, (const vector<array<float, 4>>&)shape.colors);
      add_radius(ply, shape.radius);
      add_faces(ply, (const vector<array<int, 3>>&)shape.triangles,
          (const vector<array<int, 4>>&)shape.quads);
      add_lines(ply, (const vector<array<int, 2>>&)shape.lines);
      add_points(ply, shape.points);
      if (!save_ply(filename, ply, error)) return false;
      return true;
    } else if (ext == ".obj" || ext == ".OBJ") {
      auto obj = obj_shape{};
      // TODO: remove when all as arrays
      add_positions(obj, (const vector<array<float, 3>>&)shape.positions);
      add_normals(obj, (const vector<array<float, 3>>&)shape.normals);
      add_texcoords(
          obj, (const vector<array<float, 2>>&)shape.texcoords, flip_texcoord);
      add_triangles(obj, (const vector<array<int, 3>>&)shape.triangles, 0,
          !shape.normals.empty(), !shape.texcoords.empty());
      add_quads(obj, (const vector<array<int, 4>>&)shape.quads, 0,
          !shape.normals.empty(), !shape.texcoords.empty());
      add_lines(obj, (const vector<array<int, 2>>&)shape.lines, 0,
          !shape.normals.empty(), !shape.texcoords.empty());
      add_points(obj, shape.points, 0, !shape.normals.empty(),
          !shape.texcoords.empty());
      if (!save_obj(filename, obj, error)) return false;
      return true;
    } else if (ext == ".stl" || ext == ".STL") {
      auto stl = stl_model{};
      if (!shape.lines.empty()) return shape_error();
      if (!shape.points.empty()) return shape_error();
      if (!shape.triangles.empty()) {
        add_triangles(stl, (const vector<array<int, 3>>&)shape.triangles,
            (const vector<array<float, 3>>&)shape.positions, {});
      } else if (!shape.quads.empty()) {
        auto triangles = quads_to_triangles(shape.quads);
        add_triangles(stl, (const vector<array<int, 3>>&)triangles,
            (const vector<array<float, 3>>&)shape.positions, {});
      } else {
        return shape_error();
      }
      if (!save_stl(filename, stl, error)) return false;
      return true;
    } else if (ext == ".cpp" || ext == ".CPP") {
      auto to_cpp = [](const string& name, const string& vname,
                        const auto& values) -> string {
        using T = typename std::remove_const_t<
            std::remove_reference_t<decltype(values)>>::value_type;
        if (values.empty()) return ""s;
        auto str = "auto " + name + "_" + vname + " = ";
        if constexpr (std::is_same_v<int, T>) str += "vector<int>{\n";
        if constexpr (std::is_same_v<float, T>) str += "vector<float>{\n";
        if constexpr (std::is_same_v<vec2i, T>) str += "vector<vec2i>{\n";
        if constexpr (std::is_same_v<vec2f, T>) str += "vector<vec2f>{\n";
        if constexpr (std::is_same_v<vec3i, T>) str += "vector<vec3i>{\n";
        if constexpr (std::is_same_v<vec3f, T>) str += "vector<vec3f>{\n";
        if constexpr (std::is_same_v<vec4i, T>) str += "vector<vec4i>{\n";
        if constexpr (std::is_same_v<vec4f, T>) str += "vector<vec4f>{\n";
        for (auto& value : values) {
          if constexpr (std::is_same_v<int, T> || std::is_same_v<float, T>) {
            str += std::to_string(value) + ",\n";
          } else if constexpr (std::is_same_v<vec2i, T> ||
                               std::is_same_v<vec2f, T>) {
            str += "{" + std::to_string(value.x) + "," +
                   std::to_string(value.y) + "},\n";
          } else if constexpr (std::is_same_v<vec3i, T> ||
                               std::is_same_v<vec3f, T>) {
            str += "{" + std::to_string(value.x) + "," +
                   std::to_string(value.y) + "," + std::to_string(value.z) +
                   "},\n";
          } else if constexpr (std::is_same_v<vec4i, T> ||
                               std::is_same_v<vec4f, T>) {
            str += "{" + std::to_string(value.x) + "," +
                   std::to_string(value.y) + "," + std::to_string(value.z) +
                   "," + std::to_string(value.w) + "},\n";
          } else {
            throw std::invalid_argument{"cannot print this"};
          }
        }
        str += "};\n\n";
        return str;
      };

      auto name = string{"shape"};
      auto str  = ""s;
      str += to_cpp(name, "positions", shape.positions);
      str += to_cpp(name, "normals", shape.normals);
      str += to_cpp(name, "texcoords", shape.texcoords);
      str += to_cpp(name, "colors", shape.colors);
      str += to_cpp(name, "radius", shape.radius);
      str += to_cpp(name, "points", shape.points);
      str += to_cpp(name, "lines", shape.lines);
      str += to_cpp(name, "triangles", shape.triangles);
      str += to_cpp(name, "quads", shape.quads);
      if (!save_text(filename, str, error)) return false;
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Load mesh
  shape_data load_shape(
      const string& filename, string& error, bool flip_texcoord) {
    auto shape = shape_data{};
    if (!load_shape(filename, shape, error, flip_texcoord)) return shape_data{};
    return shape;
  }
  shape_data load_shape(const string& filename, bool flip_texcoord) {
    auto error = string{};
    auto shape = shape_data{};
    if (!load_shape(filename, shape, error, flip_texcoord))
      throw io_error{error};
    return shape;
  }
  void load_shape(
      const string& filename, shape_data& shape, bool flip_texcoord) {
    auto error = string{};
    if (!load_shape(filename, shape, error, flip_texcoord))
      throw io_error{error};
  }
  void save_shape(const string& filename, const shape_data& shape,
      bool flip_texcoord, bool ascii) {
    auto error = string{};
    if (!save_shape(filename, shape, error, flip_texcoord, ascii))
      throw io_error{error};
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// TEXTURE IO
// -----------------------------------------------------------------------------
namespace yocto {

  // Loads/saves an image. Chooses hdr or ldr based on file name.
  bool load_texture(
      const string& filename, texture_data& texture, string& error) {
    auto read_error = [&]() {
      error = "cannot raed " + filename;
      return false;
    };

    auto ext = path_extension(filename);
    if (ext == ".exr" || ext == ".EXR") {
      auto pixels = (float*)nullptr;
      if (LoadEXR(&pixels, &texture.width, &texture.height, filename.c_str(),
              nullptr) != 0)
        return read_error();
      texture.linear  = true;
      texture.pixelsf = vector<vec4f>{
          (vec4f*)pixels, (vec4f*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".hdr" || ext == ".HDR") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_loadf_from_memory(buffer.data(), (int)buffer.size(),
          &texture.width, &texture.height, &ncomp, 4);
      if (!pixels) return read_error();
      texture.linear  = true;
      texture.pixelsf = vector<vec4f>{
          (vec4f*)pixels, (vec4f*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".png" || ext == ".PNG") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &texture.width, &texture.height, &ncomp, 4);
      if (!pixels) return read_error();
      texture.linear  = false;
      texture.pixelsb = vector<vec4b>{
          (vec4b*)pixels, (vec4b*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" ||
               ext == ".JPEG") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &texture.width, &texture.height, &ncomp, 4);
      if (!pixels) return read_error();
      texture.linear  = false;
      texture.pixelsb = vector<vec4b>{
          (vec4b*)pixels, (vec4b*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".tga" || ext == ".TGA") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &texture.width, &texture.height, &ncomp, 4);
      if (!pixels) return read_error();
      texture.linear  = false;
      texture.pixelsb = vector<vec4b>{
          (vec4b*)pixels, (vec4b*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".bmp" || ext == ".BMP") {
      auto buffer = vector<byte>{};
      if (!load_binary(filename, buffer, error)) return false;
      auto ncomp  = 0;
      auto pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
          &texture.width, &texture.height, &ncomp, 4);
      if (!pixels) return read_error();
      texture.linear  = false;
      texture.pixelsb = vector<vec4b>{
          (vec4b*)pixels, (vec4b*)pixels + texture.width * texture.height};
      free(pixels);
      return true;
    } else if (ext == ".ypreset" || ext == ".YPRESET") {
      if (!make_texture_preset(filename, texture, error)) return false;
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Saves an hdr image.
  bool save_texture(
      const string& filename, const texture_data& texture, string& error) {
    auto write_error = [&]() {
      error = "cannot write " + filename;
      return false;
    };

    // check for correct handling
    if (!texture.pixelsf.empty() && is_ldr_filename(filename))
      throw std::invalid_argument(
          "cannot save hdr texture to ldr file " + filename);
    if (!texture.pixelsb.empty() && is_hdr_filename(filename))
      throw std::invalid_argument(
          "cannot save ldr texture to hdr file " + filename);

    // write data
    auto stbi_write_data = [](void* context, void* data, int size) {
      auto& buffer = *(vector<byte>*)context;
      buffer.insert(buffer.end(), (byte*)data, (byte*)data + size);
    };

    auto ext = path_extension(filename);
    if (ext == ".hdr" || ext == ".HDR") {
      auto buffer = vector<byte>{};
      if (!stbi_write_hdr_to_func(stbi_write_data, &buffer, (int)texture.width,
              (int)texture.height, 4, (const float*)texture.pixelsf.data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".exr" || ext == ".EXR") {
      auto data = (byte*)nullptr;
      auto size = (size_t)0;
      if (SaveEXRToMemory((const float*)texture.pixelsf.data(),
              (int)texture.width, (int)texture.height, 4, 1, &data, &size,
              nullptr) < 0)
        return write_error();
      auto buffer = vector<byte>{data, data + size};
      free(data);
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".png" || ext == ".PNG") {
      auto buffer = vector<byte>{};
      if (!stbi_write_png_to_func(stbi_write_data, &buffer, (int)texture.width,
              (int)texture.height, 4, (const byte*)texture.pixelsb.data(),
              (int)texture.width * 4))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" ||
               ext == ".JPEG") {
      auto buffer = vector<byte>{};
      if (!stbi_write_jpg_to_func(stbi_write_data, &buffer, (int)texture.width,
              (int)texture.height, 4, (const byte*)texture.pixelsb.data(), 75))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".tga" || ext == ".TGA") {
      auto buffer = vector<byte>{};
      if (!stbi_write_tga_to_func(stbi_write_data, &buffer, (int)texture.width,
              (int)texture.height, 4, (const byte*)texture.pixelsb.data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else if (ext == ".bmp" || ext == ".BMP") {
      auto buffer = vector<byte>{};
      if (!stbi_write_bmp_to_func(stbi_write_data, &buffer, (int)texture.width,
              (int)texture.height, 4, (const byte*)texture.pixelsb.data()))
        return write_error();
      if (!save_binary(filename, buffer, error)) return false;
      return true;
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  texture_data make_texture_preset(const string& type) {
    return image_to_texture(make_image_preset(type));
  }

  // Loads/saves an image. Chooses hdr or ldr based on file name.
  texture_data load_texture(const string& filename) {
    auto error   = string{};
    auto texture = texture_data{};
    if (!load_texture(filename, texture, error)) throw io_error{error};
    return texture;
  }
  void load_texture(const string& filename, texture_data& texture) {
    auto error = string{};
    if (!load_texture(filename, texture, error)) throw io_error{error};
  }
  void save_texture(const string& filename, const texture_data& texture) {
    auto error = string{};
    if (!save_texture(filename, texture, error)) throw io_error{error};
  }

  bool make_texture_preset(
      const string& filename, texture_data& texture, string& error) {
    texture = make_texture_preset(path_basename(filename));
    if (texture.width == 0 || texture.height == 0) {
      error = "unknown preset";
      return false;
    }
    return true;
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// UTILITIES
// -----------------------------------------------------------------------------
namespace yocto {

  // make element name
  [[maybe_unused]] static string get_element_name(
      const string& name, int idx, size_t size) {
    // there are much better ways to do this, but fine for now
    auto num_str  = std::to_string(idx + 1);
    auto size_str = std::to_string(size + 1);
    while (num_str.size() < size_str.size()) num_str = "0" + num_str;
    return name + num_str;
  }

  // get name
  [[maybe_unused]] static string get_camera_name(
      const scene_data& scene, int idx) {
    if (idx < 0) return "";
    if (scene.camera_names.empty())
      return get_element_name("camera", idx, scene.cameras.size());
    return scene.camera_names[idx];
  }
  [[maybe_unused]] static string get_shape_name(
      const scene_data& scene, int idx) {
    if (idx < 0) return "";
    if (scene.shape_names.empty())
      return get_element_name("shape", idx, scene.shapes.size());
    return scene.shape_names[idx];
  }
  [[maybe_unused]] static string get_texture_name(
      const scene_data& scene, int idx) {
    if (idx < 0) return "";
    if (scene.texture_names.empty())
      return get_element_name("texture", idx, scene.textures.size());
    return scene.texture_names[idx];
  }
  [[maybe_unused]] static string get_instance_name(
      const scene_data& scene, int idx) {
    if (idx < 0) return "";
    if (scene.instance_names.empty())
      return get_element_name("instance", idx, scene.instances.size());
    return scene.instance_names[idx];
  }
  [[maybe_unused]] static string get_material_name(
      const scene_data& scene, int idx) {
    if (idx < 0) return "";
    if (scene.material_names.empty())
      return get_element_name("material", idx, scene.materials.size());
    return scene.material_names[idx];
  }

  [[maybe_unused]] static string get_camera_name(
      const scene_data& scene, const camera_data& camera) {
    return get_camera_name(scene, (int)(&camera - scene.cameras.data()));
  }
  [[maybe_unused]] static string get_shape_name(
      const scene_data& scene, const shape_data& shape) {
    return get_shape_name(scene, (int)(&shape - scene.shapes.data()));
  }
  [[maybe_unused]] static string get_texture_name(
      const scene_data& scene, const texture_data& texture) {
    return get_texture_name(scene, (int)(&texture - scene.textures.data()));
  }
  [[maybe_unused]] static string get_instance_name(
      const scene_data& scene, const instance_data& instance) {
    return get_instance_name(scene, (int)(&instance - scene.instances.data()));
  }
  [[maybe_unused]] static string get_material_name(
      const scene_data& scene, const material_data& material) {
    return get_material_name(scene, (int)(&material - scene.materials.data()));
  }

  template <typename T>
  static vector<string> make_names(const vector<T>& elements,
      const vector<string>& names, const string& prefix) {
    if (names.size() == elements.size()) return names;
    auto nnames = vector<string>(elements.size());
    for (auto idx : range(elements.size())) {
      // there are much better ways to do this, but fine for now
      auto num_str  = std::to_string(idx + 1);
      auto size_str = std::to_string(elements.size());
      while (num_str.size() < size_str.size()) num_str = "0" + num_str;
      nnames[idx] = prefix + num_str;
    }
    return nnames;
  }

  // Add missing cameras.
  void add_missing_camera(scene_data& scene) {
    if (!scene.cameras.empty()) return;
    scene.camera_names.emplace_back("camera");
    auto& camera        = scene.cameras.emplace_back();
    camera.orthographic = false;
    camera.film         = 0.036f;
    camera.aspect       = (float)16 / (float)9;
    camera.aperture     = 0;
    camera.lens         = 0.050f;
    auto bbox           = compute_bounds(scene);
    auto center         = (bbox.max + bbox.min) / 2;
    auto bbox_radius    = length(bbox.max - bbox.min) / 2;
    auto camera_dir     = vec3f{0, 0, 1};
    auto camera_dist    = bbox_radius * camera.lens /
                       (camera.film / camera.aspect);
    camera_dist *= 2.0f;  // correction for tracer camera implementation
    auto from    = camera_dir * camera_dist + center;
    auto to      = center;
    auto up      = vec3f{0, 1, 0};
    camera.frame = lookat_frame(from, to, up);
    camera.focus = length(from - to);
  }

  // Add missing radius.
  static void add_missing_radius(scene_data& scene, float radius = 0.001f) {
    for (auto& shape : scene.shapes) {
      if (shape.points.empty() && shape.lines.empty()) continue;
      if (!shape.radius.empty()) continue;
      shape.radius.assign(shape.positions.size(), radius);
    }
  }

  // Add missing caps.
  static void add_missing_caps(scene_data& scene) {
    for (auto& shape : scene.shapes) {
      if (!shape.lines.empty() && shape.ends.empty())
        shape.ends.assign(shape.positions.size(), line_end::cap);
    }
  }

  // Add missing cameras.
  void add_missing_material(scene_data& scene) {
    auto default_material = invalidid;
    for (auto& instance : scene.instances) {
      if (instance.material >= 0) continue;
      if (default_material == invalidid) {
        auto& material   = scene.materials.emplace_back();
        material.color   = {0.8f, 0.8f, 0.8f};
        default_material = (int)scene.materials.size() - 1;
      }
      instance.material = default_material;
    }
  }

  // Reduce memory usage
  static void trim_memory(scene_data& scene) {
    for (auto& shape : scene.shapes) {
      shape.points.shrink_to_fit();
      shape.lines.shrink_to_fit();
      shape.triangles.shrink_to_fit();
      shape.quads.shrink_to_fit();
      shape.positions.shrink_to_fit();
      shape.normals.shrink_to_fit();
      shape.texcoords.shrink_to_fit();
      shape.colors.shrink_to_fit();
      shape.radius.shrink_to_fit();
      shape.tangents.shrink_to_fit();
      shape.ends.shrink_to_fit();
    }
    for (auto& texture : scene.textures) {
      texture.pixelsf.shrink_to_fit();
      texture.pixelsb.shrink_to_fit();
    }
    scene.cameras.shrink_to_fit();
    scene.shapes.shrink_to_fit();
    scene.instances.shrink_to_fit();
    scene.materials.shrink_to_fit();
    scene.textures.shrink_to_fit();
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// GENERIC SCENE LOADING
// -----------------------------------------------------------------------------
namespace yocto {

  // Load/save a scene in the builtin JSON format.
  static bool load_json_scene(const string& filename, scene_data& scene,
      string& error, bool noparallel);
  static bool save_json_scene(const string& filename, const scene_data& scene,
      string& error, bool noparallel);

  // Load a scene
  bool load_scene(const string& filename, scene_data& scene, string& error,
      bool noparallel) {
    auto ext = path_extension(filename);
    if (ext == ".json" || ext == ".JSON") {
      return load_json_scene(filename, scene, error, noparallel);
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Save a scene
  bool save_scene(const string& filename, const scene_data& scene,
      string& error, bool noparallel) {
    auto ext = path_extension(filename);
    if (ext == ".json" || ext == ".JSON") {
      return save_json_scene(filename, scene, error, noparallel);
    } else {
      error = "unsupported format " + filename;
      return false;
    }
  }

  // Load/save a scene
  scene_data load_scene(const string& filename, bool noparallel) {
    auto error = string{};
    auto scene = scene_data{};
    if (!load_scene(filename, scene, error, noparallel)) throw io_error{error};
    return scene;
  }
  void load_scene(const string& filename, scene_data& scene, bool noparallel) {
    auto error = string{};
    if (!load_scene(filename, scene, error, noparallel)) throw io_error{error};
  }
  void save_scene(
      const string& filename, const scene_data& scene, bool noparallel) {
    auto error = string{};
    if (!save_scene(filename, scene, error, noparallel)) throw io_error{error};
  }

  // Make missing scene directories
  bool make_scene_directories(
      const string& filename, const scene_data& scene, string& error) {
    // make a directory if needed
    if (!make_directory(path_dirname(filename), error)) return false;
    if (!scene.shapes.empty())
      if (!make_directory(path_join(path_dirname(filename), "shapes"), error))
        return false;
    if (!scene.textures.empty())
      if (!make_directory(path_join(path_dirname(filename), "textures"), error))
        return false;
    return true;
  }

  // Make missing scene directories
  void make_scene_directories(const string& filename, const scene_data& scene) {
    auto error = string{};
    if (!make_scene_directories(filename, scene, error)) throw io_error{error};
  }

}  // namespace yocto


// -----------------------------------------------------------------------------
// JSON IO
// -----------------------------------------------------------------------------
namespace yocto {

  NLOHMANN_JSON_SERIALIZE_ENUM(
      material_type, {
                         {material_type::matte, "matte"},
                         {material_type::glossy, "glossy"},
                         {material_type::reflective, "reflective"},
                         {material_type::transparent, "transparent"},
                         {material_type::refractive, "refractive"},
                         {material_type::subsurface, "subsurface"},
                         {material_type::volumetric, "volumetric"},
                         {material_type::gltfpbr, "gltfpbr"},
                     })

  // Load a scene in the builtin JSON format.
  static bool load_json_scene(const string& filename, scene_data& scene,
      string& error, bool noparallel) {
    // open file
    auto json = json_value{};
    if (!load_json(filename, json, error)) return false;

    // check version
    if (!json.contains("asset") || !json.at("asset").contains("version") ||
        json.at("asset").at("version") != "4.2")
      return false;

    // parse json value
    auto get_opt = [](const json_value& json, const string& key, auto& value) {
      value = json.value(key, value);
    };

    // filenames
    auto shape_types       = vector<string>{};
    auto shape_filenames   = vector<string>{};
    auto shape_borders     = vector<float>{};
    auto texture_filenames = vector<string>{};

    // errors
    auto parse_error = [&filename, &error]() {
      error = "cannot parse " + filename;
      return false;
    };

    // parsing values
    try {
      if (json.contains("asset")) {
        auto& element = json.at("asset");
        get_opt(element, "copyright", scene.copyright);
        auto version = string{};
        get_opt(element, "version", version);
        if (version != "4.2" && version != "5.0") return parse_error();
      }
      if (json.contains("cameras")) {
        auto& group = json.at("cameras");
        scene.cameras.reserve(group.size());
        scene.camera_names.reserve(group.size());
        for (auto& element : group) {
          auto& camera = scene.cameras.emplace_back();
          auto& name   = scene.camera_names.emplace_back();
          get_opt(element, "name", name);
          get_opt(element, "frame", camera.frame);
          get_opt(element, "orthographic", camera.orthographic);
          get_opt(element, "lens", camera.lens);
          get_opt(element, "aspect", camera.aspect);
          get_opt(element, "film", camera.film);
          get_opt(element, "focus", camera.focus);
          get_opt(element, "aperture", camera.aperture);
        }
      }
      if (json.contains("textures")) {
        auto& group = json.at("textures");
        scene.textures.reserve(group.size());
        scene.texture_names.reserve(group.size());
        texture_filenames.reserve(group.size());
        for (auto& element : group) {
          [[maybe_unused]] auto& texture = scene.textures.emplace_back();
          auto&                  name    = scene.texture_names.emplace_back();
          auto&                  uri     = texture_filenames.emplace_back();
          get_opt(element, "name", name);
          get_opt(element, "uri", uri);
        }
      }
      if (json.contains("materials")) {
        auto& group = json.at("materials");
        scene.materials.reserve(group.size());
        scene.material_names.reserve(group.size());
        for (auto& element : json.at("materials")) {
          auto& material = scene.materials.emplace_back();
          auto& name     = scene.material_names.emplace_back();
          get_opt(element, "name", name);
          get_opt(element, "type", material.type);
          get_opt(element, "emission", material.emission);
          get_opt(element, "color", material.color);
          get_opt(element, "metallic", material.metallic);
          get_opt(element, "roughness", material.roughness);
          get_opt(element, "ior", material.ior);
          get_opt(element, "trdepth", material.trdepth);
          get_opt(element, "scattering", material.scattering);
          get_opt(element, "scanisotropy", material.scanisotropy);
          get_opt(element, "opacity", material.opacity);
          get_opt(element, "emission_tex", material.emission_tex);
          get_opt(element, "color_tex", material.color_tex);
          get_opt(element, "roughness_tex", material.roughness_tex);
          get_opt(element, "scattering_tex", material.scattering_tex);
          get_opt(element, "normal_tex", material.normal_tex);
        }
      }
      if (json.contains("shapes")) {
        auto& group = json.at("shapes");
        scene.shapes.reserve(group.size());
        scene.shape_names.reserve(group.size());
        shape_filenames.reserve(group.size());
        for (auto& element : group) {
          auto& shape  = scene.shapes.emplace_back();
          auto& name   = scene.shape_names.emplace_back();
          auto& type   = shape_types.emplace_back();
          auto& uri    = shape_filenames.emplace_back();
          auto& border = shape_borders.emplace_back();
          get_opt(element, "name", name);
          get_opt(element, "type", type);
          if (type == "point") {
            shape          = {};
            auto& position = shape.positions.emplace_back();
            auto& radius   = shape.radius.emplace_back();
            get_opt(element, "position", position);
            get_opt(element, "radius", radius);
            shape.points.push_back(0);
          } else if (type == "line") {
            shape           = {};
            auto& position1 = shape.positions.emplace_back();
            auto& radius1   = shape.radius.emplace_back();
            auto& end1      = shape.ends.emplace_back();
            get_opt(element, "position1", position1);
            get_opt(element, "radius1", radius1);
            get_opt(element, "arrow1", (bool&)end1);

            auto& position2 = shape.positions.emplace_back();
            auto& radius2   = shape.radius.emplace_back();
            auto& end2      = shape.ends.emplace_back();
            get_opt(element, "position2", position2);
            get_opt(element, "radius2", radius2);
            get_opt(element, "arrow2", (bool&)end2);

            shape.lines.push_back({0, 1});
          } else if (type == "triangle") {
            shape           = {};
            auto& position1 = shape.positions.emplace_back();
            get_opt(element, "position1", position1);

            auto& position2 = shape.positions.emplace_back();
            get_opt(element, "position2", position2);

            auto& position3 = shape.positions.emplace_back();
            get_opt(element, "position3", position3);

            shape.triangles.push_back({0, 1, 2});

            get_opt(element, "border_size", shape.border_radius);
          } else if (type == "quad") {
            shape           = {};
            auto& position1 = shape.positions.emplace_back();
            get_opt(element, "position1", position1);

            auto& position2 = shape.positions.emplace_back();
            get_opt(element, "position2", position2);

            auto& position3 = shape.positions.emplace_back();
            get_opt(element, "position3", position3);

            auto& position4 = shape.positions.emplace_back();
            get_opt(element, "position4", position4);

            shape.quads.push_back({0, 1, 2, 3});

            get_opt(element, "border_size", shape.border_radius);
          } else {
            type = "uri";
            get_opt(element, "uri", uri);
            get_opt(element, "border_size", border);
          }
        }
      }
      if (json.contains("instances")) {
        auto& group = json.at("instances");
        scene.instances.reserve(group.size());
        scene.instance_names.reserve(group.size());
        for (auto& element : group) {
          auto& instance = scene.instances.emplace_back();
          auto& name     = scene.instance_names.emplace_back();
          get_opt(element, "name", name);
          get_opt(element, "frame", instance.frame);
          get_opt(element, "shape", instance.shape);
          get_opt(element, "material", instance.material);
          get_opt(element, "border_material", instance.border_material);
        }
      }
    } catch (...) {
      return parse_error();
    }

    // prepare data
    auto dirname         = path_dirname(filename);
    auto dependent_error = [&filename, &error]() {
      error = "cannot load " + filename + " since " + error;
      return false;
    };

    // load resources
    if (noparallel) {
      // load shapes
      for (auto idx : range(scene.shapes.size())) {
        if (shape_types[idx] == "uri") {
          if (!load_shape(path_join(dirname, shape_filenames[idx]),
                  scene.shapes[idx], error, true))
            return dependent_error();
          scene.shapes[idx].border_radius = shape_borders[idx];
        }
      }
      // load textures
      for (auto idx : range(scene.textures.size())) {
        if (!load_texture(path_join(dirname, texture_filenames[idx]),
                scene.textures[idx], error))
          return dependent_error();
      }
    } else {
      // load shapes
      if (!parallel_for(
              scene.shapes.size(), error, [&](size_t idx, string& error) {
                if (shape_types[idx] == "uri") {
                  if (!load_shape(path_join(dirname, shape_filenames[idx]),
                          scene.shapes[idx], error, true))
                    return false;
                  scene.shapes[idx].border_radius = shape_borders[idx];
                  return true;
                } else
                  return true;
              }))
        return dependent_error();
      // load textures
      if (!parallel_for(
              scene.textures.size(), error, [&](size_t idx, string& error) {
                return load_texture(path_join(dirname, texture_filenames[idx]),
                    scene.textures[idx], error);
              }))
        return dependent_error();
    }

    // fix scene
    add_missing_camera(scene);
    add_missing_radius(scene);
    add_missing_caps(scene);
    trim_memory(scene);

    // done
    return true;
  }

  // Save a scene in the builtin JSON format.
  static bool save_json_scene(const string& filename, const scene_data& scene,
      string& error, bool noparallel) {
    // helpers to handel old code paths
    auto add_object = [](json_value& json, const string& name) -> json_value& {
      auto& item = json[name];
      item       = json_value::object();
      return item;
    };
    auto add_array = [](json_value& json, const string& name) -> json_value& {
      auto& item = json[name];
      item       = json_value::array();
      return item;
    };
    auto append_object = [](json_value& json) -> json_value& {
      auto& item = json.emplace_back();
      item       = json_value::object();
      return item;
    };
    auto set_val = [](json_value& json, const string& name, const auto& value,
                       const auto& def) {
      if (value == def) return;
      json[name] = value;
    };
    auto reserve_values = [](json_value& json, size_t size) {
      json.get_ptr<json_value::array_t*>()->reserve(size);
    };

    // names
    auto get_name = [](const vector<string>& names, size_t idx) -> string {
      return (idx < names.size()) ? names[idx] : "";
    };
    auto get_filename = [](const vector<string>& names, size_t idx,
                            const string& basename,
                            const string& extension) -> string {
      if (idx < names.size()) {
        return basename + "s/" + names[idx] + extension;
      } else {
        return basename + "s/" + basename + std::to_string(idx) + extension;
      }
    };

    // filenames
    auto shape_filenames   = vector<string>(scene.shapes.size());
    auto texture_filenames = vector<string>(scene.textures.size());
    for (auto idx : range(shape_filenames.size())) {
      shape_filenames[idx] = get_filename(
          scene.shape_names, idx, "shape", ".ply");
    }
    for (auto idx : range(texture_filenames.size())) {
      texture_filenames[idx] = get_filename(scene.texture_names, idx, "texture",
          (scene.textures[idx].pixelsf.empty() ? ".png" : ".hdr"));
    }

    // save json file
    auto json = json_value::object();

    // asset
    {
      auto& element = add_object(json, "asset");
      set_val(element, "copyright", scene.copyright, "");
      set_val(element, "generator",
          "Yocto/GL - https://github.com/xelatihy/yocto-gl"s, ""s);
      set_val(element, "version", "4.2"s, ""s);
    }

    if (!scene.cameras.empty()) {
      auto  default_ = camera_data{};
      auto& group    = add_array(json, "cameras");
      reserve_values(group, scene.cameras.size());
      for (auto&& [idx, camera] : enumerate(scene.cameras)) {
        auto& element = append_object(group);
        set_val(element, "name", get_name(scene.camera_names, idx), "");
        set_val(element, "frame", camera.frame, default_.frame);
        set_val(element, "orthographic", camera.orthographic,
            default_.orthographic);
        set_val(element, "lens", camera.lens, default_.lens);
        set_val(element, "aspect", camera.aspect, default_.aspect);
        set_val(element, "film", camera.film, default_.film);
        set_val(element, "focus", camera.focus, default_.focus);
        set_val(element, "aperture", camera.aperture, default_.aperture);
      }
    }

    if (!scene.textures.empty()) {
      auto& group = add_array(json, "textures");
      reserve_values(group, scene.textures.size());
      for (auto&& [idx, texture] : enumerate(scene.textures)) {
        auto& element = append_object(group);
        set_val(element, "name", get_name(scene.texture_names, idx), "");
        set_val(element, "uri", texture_filenames[idx], ""s);
      }
    }

    if (!scene.materials.empty()) {
      auto  default_ = material_data{};
      auto& group    = add_array(json, "materials");
      reserve_values(group, scene.materials.size());
      for (auto&& [idx, material] : enumerate(scene.materials)) {
        auto& element = append_object(group);
        set_val(element, "name", get_name(scene.material_names, idx), "");
        set_val(element, "type", material.type, default_.type);
        set_val(element, "emission", material.emission, default_.emission);
        set_val(element, "color", material.color, default_.color);
        set_val(element, "metallic", material.metallic, default_.metallic);
        set_val(element, "roughness", material.roughness, default_.roughness);
        set_val(element, "ior", material.ior, default_.ior);
        set_val(element, "trdepth", material.trdepth, default_.trdepth);
        set_val(
            element, "scattering", material.scattering, default_.scattering);
        set_val(element, "scanisotropy", material.scanisotropy,
            default_.scanisotropy);
        set_val(element, "opacity", material.opacity, default_.opacity);
        set_val(element, "emission_tex", material.emission_tex,
            default_.emission_tex);
        set_val(element, "color_tex", material.color_tex, default_.color_tex);
        set_val(element, "roughness_tex", material.roughness_tex,
            default_.roughness_tex);
        set_val(element, "scattering_tex", material.scattering_tex,
            default_.scattering_tex);
        set_val(
            element, "normal_tex", material.normal_tex, default_.normal_tex);
      }
    }

    if (!scene.shapes.empty()) {
      auto& group = add_array(json, "shapes");
      reserve_values(group, scene.shapes.size());
      for (auto&& [idx, shape] : enumerate(scene.shapes)) {
        auto& element = append_object(group);
        set_val(element, "name", get_name(scene.shape_names, idx), "");
        set_val(element, "uri", shape_filenames[idx], "");
      }
    }

    if (!scene.instances.empty()) {
      auto  default_ = instance_data{};
      auto& group    = add_array(json, "instances");
      reserve_values(group, scene.instances.size());
      for (auto&& [idx, instance] : enumerate(scene.instances)) {
        auto& element = append_object(group);
        set_val(element, "name", get_name(scene.instance_names, idx), "");
        set_val(element, "frame", instance.frame, default_.frame);
        set_val(element, "shape", instance.shape, default_.shape);
        set_val(element, "material", instance.material, default_.material);
      }
    }

    // save json
    if (!save_json(filename, json, error)) return false;

    // prepare data
    auto dirname         = path_dirname(filename);
    auto dependent_error = [&filename, &error]() {
      error = "cannot save " + filename + " since " + error;
      return false;
    };

    // dirname
    if (noparallel) {
      // save shapes
      for (auto idx : range(scene.shapes.size())) {
        if (!save_shape(path_join(dirname, shape_filenames[idx]),
                scene.shapes[idx], error, true))
          return dependent_error();
      }
      // save textures
      for (auto idx : range(scene.textures.size())) {
        if (!save_texture(path_join(dirname, texture_filenames[idx]),
                scene.textures[idx], error))
          return dependent_error();
      }
    } else {
      // save shapes
      if (!parallel_for(
              scene.shapes.size(), error, [&](auto idx, string& error) {
                return save_shape(path_join(dirname, shape_filenames[idx]),
                    scene.shapes[idx], error, true);
              }))
        return dependent_error();
      // save textures
      if (!parallel_for(
              scene.textures.size(), error, [&](auto idx, string& error) {
                return save_texture(path_join(dirname, texture_filenames[idx]),
                    scene.textures[idx], error);
              }))
        return dependent_error();
    }

    // done
    return true;
  }

}  // namespace yocto

// -----------------------------------------------------------------------------
// JSON CLI
// -----------------------------------------------------------------------------
namespace yocto {

  // Using directive
  using ordered_json = nlohmann::ordered_json;

  // Parse command line arguments to Json without schema
  static bool cli_to_json_value(ordered_json& json, const string& arg) {
    if (arg.empty()) throw std::invalid_argument("should not have gotten here");
    json = ordered_json::parse(arg, nullptr, false);
    if (json.is_discarded()) json = arg;
    return true;
  }
  static pair<bool, int> cli_to_json_option(
      ordered_json& json, const vector<string>& args, int pos) {
    if (pos >= args.size() || args[pos].find("--") == 0) {
      json = true;
      return {true, pos};
    } else {
      while (pos < (int)args.size() && args[pos].find("--") != 0) {
        if (json.is_array()) {
          if (!cli_to_json_value(json.emplace_back(), args[pos++]))
            return {false, pos};
        } else if (json.is_null()) {
          if (!cli_to_json_value(json, args[pos++])) return {false, pos};
        } else {
          auto item = json;
          json      = ordered_json::array();
          json.push_back(item);
          if (!cli_to_json_value(json.emplace_back(), args[pos++]))
            return {false, pos};
        }
      }
      return {true, pos};
    }
  }
  static bool cli_to_json_command(
      ordered_json& json, const vector<string>& args, int pos) {
    if (pos >= args.size()) return true;
    if (args[pos].find("--") == 0) {
      while (pos < (int)args.size() && args[pos].find("--") == 0) {
        auto result = cli_to_json_option(
            json[args[pos].substr(2)], args, pos + 1);
        if (!result.first) return false;
        pos = result.second;
      }
      return true;
    } else {
      return cli_to_json_command(json[args[pos]], args, pos + 1);
    }
  }
  bool cli_to_json(ordered_json& json, const vector<string>& args) {
    return cli_to_json_command(json, args, 1);
  }
  bool cli_to_json(ordered_json& json, int argc, const char** argv) {
    return cli_to_json(json, vector<string>{argv, argv + argc});
  }

  // Validate Cli Json against a schema
  bool validate_cli(const ordered_json& json, const ordered_json& schema);

  // Get Cli usage from Json
  string cli_usage(const ordered_json& json, const ordered_json& schema);

}  // namespace yocto

// -----------------------------------------------------------------------------
// HELPERS FOR JSON MANIPULATION
// -----------------------------------------------------------------------------
namespace yocto {

  // Validate a Json value againt a schema. Returns the first error found.
  void validate_json(const json_value& json, const json_value& schema);
  bool validate_json(
      const json_value& json, const json_value& schema, string& error);

  // Converts command line arguments to Json. Never throws since a conversion
  // is always possible in our conventions. Validation is done using a schema.
  json_value make_json_cli(const vector<string>& args) {
    // init json
    auto json = json_value{};
    if (args.size() < 2) return json;

    // split into commans and options; use spans when available for speed
    auto commands = vector<string>{};
    auto options  = vector<pair<string, vector<string>>>{};
    for (auto& arg : args) {
      if (arg.find("--") == 0) {
        // start option
        options.push_back({arg.substr(2), {}});
      } else if (!options.empty()) {
        // add value
        options.back().second.push_back(arg);
      } else {
        // add command
        commands.push_back(arg);
      }
    }

    // build commands
    auto current = &json;
    for (auto& command : commands) {
      auto& json      = *current;
      json["command"] = command;
      json[command]   = json_value::object();
      current         = &json[command];
    }

    // build options
    for (auto& [name, values] : options) {
      auto& json = *current;
      if (values.empty()) {
        json[name] = true;
      } else {
        json[name] = json_value::array();
        for (auto& value : values) {
          if (value == "true") {
            json[name].push_back(true);
          } else if (value == "false") {
            json[name].push_back(false);
          } else if (value == "null") {
            json[name].push_back(nullptr);
          } else if (std::isdigit((int)value[0]) || value[0] == '-' ||
                     value[0] == '+') {
            try {
              if (value.find('.') != string::npos) {
                json[name].push_back(std::stod(value));
              } else if (value.find('-') == 0) {
                json[name].push_back(std::stoll(value));
              } else {
                json[name].push_back(std::stoull(value));
              }
            } catch (...) {
              json[name].push_back(value);
            }
          } else {
            json[name].push_back(value);
          }
        }
        if (values.size() == 1) {
          json[name] = json[name].front();
        }
      }
    }

    // done
    return json;
  }
  json_value make_json_cli(int argc, const char** argv) {
    return make_json_cli(vector<string>{argv, argv + argc});
  }

  // Validates a JSON against a schema including CLI constraints.
  json_value validate_json_cli(const vector<string>& args);
  json_value validate_json_cli(int argc, const char** argv);

  // Helpers for creating schemas

}  // namespace yocto
