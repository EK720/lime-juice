//
// lime-juice: C++ port of Tomyun's "Juice" de/recompiler for PC-98 games
// Copyright (C) 2026 Fuzion
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "cli_utils.h"
#include "codec/gp4/gp4.h"
#include "codec/gpc/gpc.h"
#include "codec/gpa/gpa.h"
#include "image/png_io.h"
#include "image/gif_io.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* TITLE = "juice-img";
static const char* VERSION = "v0.3.0 (lime-juice img ver.)";

// detect image format from file contents
enum class ImageFormat { GP4, GPC, GPA, Unknown };

static ImageFormat detect_format(const std::vector<uint8_t>& data,
                                  const std::string& path) {

    // check for GPC/GPA signatures (reliable magic bytes)
    if (data.size() >= 16) {
        std::string sig(data.begin(), data.begin() + 15);

        if (sig == gpc::signature) {
            return ImageFormat::GPC;
        }

        if (sig == gpa::signature) {
            return ImageFormat::GPA;
        }
    }

    // GP4 has no magic bytes; trust the file extension
    std::string ext = get_ext(path);

    if (ext == ".gp4") {
        return ImageFormat::GP4;
    }

    return ImageFormat::Unknown;
}

// case-insensitive string comparison for filename matching
static bool iequals(const std::string& a, const std::string& b) {

    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); i++) {

        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }

    return true;
}

// try to find a palette source for a GPA file.
// goes up one directory and recursively searches for a GPC file with
// the same stem, since game directory layouts vary widely.
static std::string find_palette_for_gpa(const std::string& gpa_path) {
    fs::path p(gpa_path);
    std::string target = p.stem().string() + ".gpc";
    fs::path search_root = p.parent_path().parent_path();

    if (search_root.empty() || !fs::exists(search_root)) {
        return "";
    }

    for (const auto& entry : fs::recursive_directory_iterator(search_root)) {

        if (entry.is_regular_file() && iequals(entry.path().filename().string(), target)) {
            return entry.path().string();
        }
    }

    return "";
}

// read palette from a GPC file (first 16 entries)
static std::array<Rgb, 16> read_gpc_palette(const std::string& path) {
    auto data = read_file(path);

    if (data.size() < 0x1C) {
        throw std::runtime_error("palette source too small: " + path);
    }

    uint32_t pal_ptr = data[0x14] | (data[0x15] << 8) |
                       (data[0x16] << 16) | (data[0x17] << 24);

    std::array<Rgb, 16> palette = {};

    if (pal_ptr > 0 && pal_ptr + 36 <= data.size()) {

        for (int i = 0; i < 16; i++) {
            size_t off = pal_ptr + 4 + i * 2;
            uint16_t entry = data[off] | (data[off + 1] << 8);
            palette[i] = gpc_palette_to_rgb(entry);
        }
    }

    return palette;
}

static void print_usage() {
    std::cerr <<
        "usage: " << TITLE << " <command> [options] <files...>\n"
        "\n"
        "commands:\n"
        "  -d, --decode    decode GP4/GPC to PNG, GPA to GIF\n"
        "  -c, --compile   compile (encode) PNG to GP4/GPC, GIF to GPA\n"
        "  -v, --version   show version\n"
        "\n"
        "options:\n"
        "  -f, --force         overwrite existing output files\n"
        "  -o, --output PATH   output file path (single file only)\n"
        "  -F, --format FMT    target format for compile: gp4, gpc, gpa\n"
        "  -p, --palette PATH  palette source (GPC file) for GPA decode\n"
        "  -W, --force-width N canvas width override for GP4 decode (default 640)\n"
        "  -x, --force-x N     x position override for compile\n"
        "  -y, --force-y N     y position override for compile\n"
    ;
}

enum class Command { None, Decode, Encode, ShowVersion };

static void decode_file(const std::string& path, bool force,
                        const std::string& output_override = "",
                        const std::string& palette_path = "",
                        int force_width = 0) {
    auto data = read_file(path);
    auto fmt = detect_format(data, path);
    std::string stem = get_stem(path);

    std::string out;
    std::string out_ext;
    bool missing_palette = false;

    if (force_width > 0 && fmt != ImageFormat::GP4) {
        print_color("b-white", stem);
        println_color("b-yellow", " (warning: --force-width only applies to GP4 files, ignoring)");
    }

    if (fmt == ImageFormat::GP4) {

        // warn if the header width is not a multiple of 4 (re-encoding will fail)
        if (data.size() >= 8) {
            uint16_t raw_w = ((data[4] << 8) | data[5]) + 1;

            if (raw_w % 4 != 0) {
                print_color("b-white", stem);
                println_color("b-yellow", " (warning: width " + std::to_string(raw_w) +
                    " is not a multiple of 4, re-encoding as GP4 will require resizing)");
            }
        }

        auto img = gp4::decode(data, force_width > 0 ? force_width : 640);
        out = output_override.empty() ? replace_ext(path, ".png") : output_override;
        out_ext = ".png";

        if (!force && fs::exists(out)) {
            throw std::runtime_error("output file already exists (use -f to overwrite)");
        }

        save_png(out, img);
    } else if (fmt == ImageFormat::GPC) {
        auto img = gpc::decode(data);
        out = output_override.empty() ? replace_ext(path, ".png") : output_override;
        out_ext = ".png";

        if (!force && fs::exists(out)) {
            throw std::runtime_error("output file already exists (use -f to overwrite)");
        }

        save_png(out, img);
    } else if (fmt == ImageFormat::GPA) {
        auto frames = gpa::decode(data);

        // if no embedded palette, try to find one from a GPC file
        bool needs_palette = true;

        for (const auto& pal : frames[0].palette) {

            if (pal.r != 0 || pal.g != 0 || pal.b != 0) {
                needs_palette = false;
                break;
            }
        }

        if (needs_palette) {
            std::string pal_source = palette_path;

            if (pal_source.empty()) {
                pal_source = find_palette_for_gpa(path);
            }

            if (!pal_source.empty()) {
                auto palette = read_gpc_palette(pal_source);

                for (auto& frame : frames) {
                    frame.palette = palette;
                }
            } else {
                missing_palette = true;
            }
        }

        out = output_override.empty() ? replace_ext(path, ".gif") : output_override;
        out_ext = ".gif";

        if (!force && fs::exists(out)) {
            throw std::runtime_error("output file already exists (use -f to overwrite)");
        }

        bool has_meta = save_gif(out, frames);

        print_color("b-white", get_stem(out));
        print_color("b-yellow", get_ext(out));

        if (missing_palette) {
            println_color("b-yellow", " (warning: no palette found, use -p to specify a parent GPC file)");
        } else if (has_meta) {
            println_color("b-yellow", " (warning: last frame stores original negative offsets for re-encoding, do not edit it)");
        } else {
            println_color("b-green", get_ext(out));
        }

        return;
    } else {
        throw std::runtime_error("unrecognized image format");
    }

    print_color("b-white", get_stem(out));
    println_color("b-green", get_ext(out));
}

static void encode_file(const std::string& path, const std::string& format_str,
                        bool force, const std::string& output_override,
                        int force_x, int force_y) {
    std::string ext = get_ext(path);
    std::string stem = get_stem(path);
    std::string out_ext = format_str.empty()
        ? get_ext(output_override) : "." + format_str;

    if (ext == ".png") {
        auto img = load_png(path);

        if (force_x >= 0) { img.x = static_cast<int16_t>(force_x); }
        if (force_y >= 0) { img.y = static_cast<int16_t>(force_y); }

        std::string out;

        if (out_ext == ".gp4") {
            out = output_override.empty()
                ? replace_ext(path, ".gp4") : output_override;
        } else if (out_ext == ".gpc" || out_ext.empty()) {
            out = output_override.empty()
                ? replace_ext(path, ".gpc") : output_override;
        } else {
            throw std::runtime_error("unsupported output format: " + out_ext);
        }

        if (!force && fs::exists(out)) {
            throw std::runtime_error("output file already exists (use -f to overwrite)");
        }

        if (out_ext == ".gp4") {

            // GP4 requires width to be a multiple of 4; truncate with warning
            if (img.w % 4 != 0) {
                uint16_t old_w = img.w;
                img.w &= ~3;
                img.pixels.resize(static_cast<size_t>(img.w) * img.h);

                for (int row = img.h - 1; row >= 1; row--) {
                    std::memmove(&img.pixels[row * img.w],
                                 &img.pixels[row * old_w],
                                 img.w);
                }

                print_color("b-white", stem);
                println_color("b-yellow", get_ext(out) + " (warning: width " + std::to_string(old_w) +
                    " is not a multiple of 4, truncating to " + std::to_string(img.w) + " for GP4)");
                write_file(out, gp4::encode(img));
                return;
            }

            write_file(out, gp4::encode(img));
        } else {
            write_file(out, gpc::encode(img));
        }

        print_color("b-white", stem);
        println_color("b-green", get_ext(out));
    } else if (ext == ".gif") {
        std::string out = output_override.empty()
            ? replace_ext(path, ".gpa") : output_override;

        if (!force && fs::exists(out)) {
            throw std::runtime_error("output file already exists (use -f to overwrite)");
        }

        auto frames = load_gif(path);

        if (force_x >= 0 || force_y >= 0) {

            for (auto& frame : frames) {

                if (force_x >= 0) { frame.x = static_cast<int16_t>(force_x); }
                if (force_y >= 0) { frame.y = static_cast<int16_t>(force_y); }
            }
        }

        write_file(out, gpa::encode(frames));
        print_color("b-white", stem);
        println_color("b-green", get_ext(out));
    } else {
        throw std::runtime_error("unsupported input format: " + ext);
    }
}

static int run_cli(int argc, char* argv[]) {
    enable_ansi_console();

    Command command = Command::None;
    bool force = false;
    std::string output_path;
    std::string format_str;
    std::string palette_path;
    int force_width = 0;
    int force_x = -1;
    int force_y = -1;
    std::vector<std::string> file_args;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-d" || arg == "--decode") {
            command = Command::Decode;
        } else if (arg == "-c" || arg == "--compile") {
            command = Command::Encode;
        } else if (arg == "-v" || arg == "--version") {
            command = Command::ShowVersion;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_path = argv[++i];
        } else if ((arg == "-F" || arg == "--format") && i + 1 < argc) {
            format_str = argv[++i];
        } else if ((arg == "-p" || arg == "--palette") && i + 1 < argc) {
            palette_path = argv[++i];
        } else if ((arg == "-W" || arg == "--force-width") && i + 1 < argc) {
            force_width = std::stoi(argv[++i]);
        } else if ((arg == "-x" || arg == "--force-x") && i + 1 < argc) {
            force_x = std::stoi(argv[++i]);

            if (force_x < 0 || force_x > 65535) {
                std::cerr << "--force-x must be 0-65535" << std::endl;
                return 1;
            }
        } else if ((arg == "-y" || arg == "--force-y") && i + 1 < argc) {
            force_y = std::stoi(argv[++i]);

            if (force_y < 0 || force_y > 65535) {
                std::cerr << "--force-y must be 0-65535" << std::endl;
                return 1;
            }
        } else if (arg == "-f" || arg == "--force") {
            force = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg[0] == '-') {
            std::cerr << "unknown option: " << arg << std::endl;
            print_usage();
            return 1;
        } else {
            file_args.push_back(arg);
        }
    }

    if (command == Command::ShowVersion) {
        std::cout << TITLE << " " << VERSION << std::endl;
        return 0;
    }

    if (command == Command::None) {
        print_usage();
        return 1;
    }

    auto paths = expand_globs(file_args);

    if (paths.empty()) {
        std::cerr << "no input files" << std::endl;
        return 1;
    }

    if (!output_path.empty() && paths.size() > 1) {
        std::cerr << "-o cannot be used with multiple input files" << std::endl;
        return 1;
    }

    int errors = 0;

    for (const auto& path : paths) {

        try {

            if (command == Command::Decode) {
                decode_file(path, force, output_path, palette_path, force_width);
            } else if (command == Command::Encode) {
                encode_file(path, format_str, force, output_path, force_x, force_y);
            }

        } catch (const std::exception& e) {
            print_color("b-white", get_stem(path));
            print_color("b-red", " ! ");
            println_color("b-red", e.what());
            errors++;
        }
    }

    return errors > 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
