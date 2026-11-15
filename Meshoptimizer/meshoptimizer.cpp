//-------------------------------------------------------------------------------------
// File: meshoptimizer.cpp
//
// Cross-platform command line tool that reorders faces and vertices to improve
// post-transform vertex cache hit rate without changing the rendered geometry.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#include "MeshCacheOpt.h"
#include "ObjLoader.h"

#include "DirectXMesh.h"

#include <cctype>
#include <exception>
#include <iostream>
#include <string>

using DirectX::OPTFACES_V_DEFAULT;

namespace
{
    struct Options
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::string           algorithm = "hoppe";
        size_t                cacheSize = OPTFACES_V_DEFAULT;
        bool                  attributeSort = true;
        bool                  forceIndex32  = false;
        bool                  statsOnly     = false;
        bool                  showHelp      = false;
    };

    void PrintUsage()
    {
        std::cout <<
            "meshoptimizer - vertex cache optimization tool\n"
            "\n"
            "Usage: meshoptimizer [options] <input-file> [-o <output-file>]\n"
            "\n"
            "Input formats:\n"
            "  .obj      WaveFront OBJ\n"
            "  .meshbin  Raw mesh container (see MeshCacheOpt.h)\n"
            "\n"
            "Options:\n"
            "  -a, --algorithm <hoppe|lru>\n"
            "                         Face reorder algorithm. Hoppe tipsification needs\n"
            "                         position data; LRU (Forsyth) is index-only.\n"
            "                         Default: hoppe\n"
            "      --cache-size <n>  FIFO cache size used for ACMR/ATVR stats.\n"
            "                         Default: 12\n"
            "      --no-sort         Do not group faces by attribute id first.\n"
            "      --ib32            Force 32-bit indices in the output .meshbin.\n"
            "      --stats           Print cache stats and exit without writing.\n"
            "  -o <file>             Output path. Defaults to the input name with\n"
            "                         an .opt.meshbin suffix.\n"
            "  -h, --help            Show this help.\n";
    }

    bool ParseArgs(int argc, char** argv, Options& options)
    {
        for (int arg = 1; arg < argc; ++arg)
        {
            const std::string token = argv[arg];

            auto needValue = [&](const char* name) -> const char* {
                if (arg + 1 >= argc)
                {
                    std::cerr << "error: option " << name << " requires a value\n";
                    return nullptr;
                }
                return argv[++arg];
            };

            if (token == "-h" || token == "--help")
            {
                PrintUsage();
                options.showHelp = true;
                return true;
            }
            if (token == "-a" || token == "--algorithm")
            {
                const char* value = needValue("--algorithm");
                if (!value)
                    return false;
                options.algorithm = value;
            }
            else if (token == "--cache-size")
            {
                const char* value = needValue("--cache-size");
                if (!value)
                    return false;

                try
                {
                    const int parsed = std::stoi(value);
                    if (parsed <= 0)
                    {
                        std::cerr << "error: --cache-size must be positive\n";
                        return false;
                    }
                    options.cacheSize = static_cast<size_t>(parsed);
                }
                catch (const std::exception&)
                {
                    std::cerr << "error: invalid --cache-size value '" << value << "'\n";
                    return false;
                }
            }
            else if (token == "--no-sort")
            {
                options.attributeSort = false;
            }
            else if (token == "--ib32")
            {
                options.forceIndex32 = true;
            }
            else if (token == "--stats")
            {
                options.statsOnly = true;
            }
            else if (token == "-o")
            {
                const char* value = needValue("-o");
                if (!value)
                    return false;
                options.output = std::filesystem::path(value);
            }
            else if (!token.empty() && token[0] == '-')
            {
                std::cerr << "error: unknown option " << token << "\n";
                return false;
            }
            else if (!options.input.empty())
            {
                std::cerr << "error: multiple input files are not supported\n";
                return false;
            }
            else
            {
                options.input = std::filesystem::path(token);
            }
        }

        if (options.algorithm != "hoppe" && options.algorithm != "lru")
        {
            std::cerr << "error: unknown algorithm '" << options.algorithm
                      << "' (expected hoppe or lru)\n";
            return false;
        }

        if (options.input.empty())
        {
            std::cerr << "error: no input file specified\n";
            return false;
        }

        return true;
    }

    bool LoadInput(const std::filesystem::path& path, meshopt::MeshData& mesh, std::string& error)
    {
        std::string ext = path.extension().string();
        for (char& ch : ext)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (ext == ".obj")
            return meshopt::LoadObjMesh(path, mesh, error);

        if (ext == ".meshbin")
            return meshopt::LoadMesh(path, mesh, error);

        error = "unsupported input extension '" + ext + "' (expected .obj or .meshbin)";
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options))
        return 1;

    if (options.showHelp)
        return 0;

    meshopt::MeshData mesh;
    std::string error;

    try
    {
        if (!LoadInput(options.input, mesh, error))
        {
            std::cerr << "error: " << error << ": " << options.input.string() << "\n";
            return 1;
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << ": " << options.input.string() << "\n";
        return 1;
    }

    std::cout << "Loaded " << mesh.GetFaceCount() << " faces, "
              << mesh.GetVertexCount() << " vertices (stride "
              << mesh.vertexStride << ")\n";

    try
    {
        const meshopt::OptimizationReport report =
            meshopt::OptimizeMesh(mesh, options.algorithm, options.cacheSize, options.attributeSort);
        meshopt::PrintReport(report);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

    if (options.statsOnly)
        return 0;

    std::filesystem::path output = options.output;
    if (output.empty())
    {
        output = options.input;
        output.replace_extension(".opt.meshbin");
    }

    if (!meshopt::SaveMesh(output, mesh, options.forceIndex32, error))
    {
        std::cerr << "error: " << error << ": " << output.string() << "\n";
        return 1;
    }

    std::cout << "Wrote optimized mesh: " << output.string() << "\n";
    return 0;
}
