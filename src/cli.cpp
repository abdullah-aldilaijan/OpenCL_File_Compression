// Command-line front end (replaces running gpu_zip.py / opencl_lz.py / hardware.py directly).
#include <cstdio>
#include <cstdlib>

#include "common.h"
#include "ocl.h"

static int usage() {
    std::fputs(
        "OpenCL ZIP prototype (C++)\n\n"
        "  oclzip devices\n"
        "      List the CPU and usable OpenCL GPUs.\n"
        "  oclzip compress <file-or-folder> <new.zip> [--device NAME]\n"
        "      Create a standard ZIP. NAME is part of a GPU name (e.g. NVIDIA, Intel), CPU,\n"
        "      or ALL to use the CPU and every GPU at the same time.\n"
        "      Default: the best available GPU.\n"
        "  oclzip decompress <archive> <new-target> [--device NAME]\n"
        "      .zip -> new folder (CPU). .ocpk -> new folder. .folder.oclr -> new folder,\n"
        "      .oclr -> file (selected device).\n"
        "  oclzip pack <file-or-folder> <new.ocpk> [--level fast|max|store|gpu|all|deflate]\n"
        "      Solid archive: all files combined into one stream, compressed in blocks on all CPU\n"
        "      threads (fast = XPRESS+Huffman, max = LZMS). Repeated data is stored once.\n"
        "      gpu / all / deflate: the ZIP encoder's DEFLATE on the GPU, GPU + CPU, or the CPU.\n"
        "  oclzip unpack <archive.ocpk> <new-folder>\n"
        "  oclzip test <archive.ocpk>\n"
        "      Decompress everything and check every CRC-32 without writing files.\n",
        stderr);
    return 2;
}

static std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 32) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += char(c);
    }
    return out + "\"";
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> args;
    std::string device, level;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--device") {
            if (i + 1 >= argc) return usage();
            device = utf8(argv[++i]);
        } else if (a == L"--level") {
            if (i + 1 >= argc) return usage();
            level = utf8(argv[++i]);
        } else if (a.rfind(L"--device=", 0) == 0) {
            device = utf8(a.substr(9));
        } else {
            args.push_back(a);
        }
    }
    if (args.empty()) return usage();
    try {
        if (args[0] == L"devices" && args.size() == 1) {
            std::vector<std::string> notices;
            auto options = discover_devices(notices);
            std::string text = "{\n  \"devices\": [";
            for (size_t i = 0; i < options.size(); i++)
                text += std::string(i ? "," : "") + "\n    " + json_string(options[i].label);
            text += "\n  ],\n  \"notices\": [";
            for (size_t i = 0; i < notices.size(); i++)
                text += std::string(i ? "," : "") + "\n    " + json_string(notices[i]);
            text += notices.empty() ? "]\n}" : "\n  ]\n}";
            say(text);
            return 0;
        }
        if (args[0] == L"selftest") {  // hidden: fast CPU encoder vs reference, optional real files
            return selftest_encoders(std::vector<std::wstring>(args.begin() + 1, args.end()));
        }
        if (args[0] == L"benchgpu" && args.size() >= 2) {  // hidden: encoder versions on one in-memory batch
            uint32_t chunks = 8192;
            int reps = 7;
            if (const wchar_t* v = _wgetenv(L"BENCH_CHUNKS")) chunks = uint32_t(std::max(16, _wtoi(v)));
            if (const wchar_t* v = _wgetenv(L"BENCH_REPS")) reps = std::max(1, _wtoi(v));
            return bench_encoders(std::vector<std::wstring>(args.begin() + 1, args.end()), chunks, reps);
        }
        double start = seconds_now();
        if (args[0] == L"test" && args.size() == 2) {
            restore_pack(args[1], L"");
            return 0;
        }
        if (args.size() != 3) return usage();
        if (args[0] == L"compress") {
            create_zip(args[1], args[2], device);
        } else if (args[0] == L"pack") {
            create_pack(args[1], args[2], level);
        } else if (args[0] == L"unpack") {
            restore_pack(args[1], args[2]);
        } else if (args[0] == L"decompress") {
            restore_any(args[1], args[2], device);
        } else {
            return usage();
        }
        char elapsed[64];
        std::snprintf(elapsed, sizeof elapsed, "%.3fs", seconds_now() - start);
        say(std::string("Total elapsed: ") + elapsed);
        return 0;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
