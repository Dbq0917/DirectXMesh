//-------------------------------------------------------------------------------------
// File: MeshCacheOpt.h
//
// Cross-platform vertex cache optimizer for pre-transform/post-transform caches.
//
// The optimizer only permutes faces and vertices. Vertex data is treated as an
// opaque byte stream, so positions, normals, UVs, colors and any other vertex
// channels are preserved byte for byte. Rendered geometry is unchanged.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace meshopt
{
// On-disk container: little-endian, tightly packed.
//
// header      -> MeshFileHeader
// indices     -> index32 ? uint32_t : uint16_t, count = faceCount * 3
// attributes  -> uint32_t, count = faceCount (only when flags & MESH_FLAG_HAS_ATTRIBUTES)
// vertices    -> raw bytes, count = vertexCount * vertexStride
constexpr uint32_t MESH_FILE_MAGIC   = 0x4853454D; // 'MESH'
constexpr uint32_t MESH_FILE_VERSION = 1;

constexpr uint32_t MESH_FLAG_HAS_ATTRIBUTES = 0x00000001;
constexpr uint32_t MESH_FLAG_INDEX_32       = 0x00000002;

#pragma pack(push, 1)
struct MeshFileHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t faceCount;
    uint32_t vertexCount;
    uint32_t vertexStride;
    // Byte offset of the position channel inside a vertex. Required only for
    // Hoppe optimization, which needs geometric adjacency. Set to UINT32_MAX
    // when positions are not available in the stream.
    uint32_t positionOffset;
};
#pragma pack(pop)

static_assert(sizeof(MeshFileHeader) == 28, "Mesh file header layout mismatch");

struct MeshData
{
    std::vector<uint32_t> indices;
    std::vector<uint32_t> attributes;
    std::vector<uint8_t>  vertices;
    uint32_t              vertexStride   = 0;
    uint32_t              positionOffset = UINT32_MAX;

    uint32_t GetFaceCount() const noexcept
    {
        return static_cast<uint32_t>(indices.size() / 3);
    }

    uint32_t GetVertexCount() const noexcept
    {
        if (vertexStride == 0)
            return 0;

        return static_cast<uint32_t>(vertices.size() / vertexStride);
    }

    bool HasAttributes() const noexcept
    {
        return !attributes.empty();
    }
};

inline uint16_t ReadU16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0])
                                 | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t ReadU32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0])
           | (static_cast<uint32_t>(p[1]) << 8)
           | (static_cast<uint32_t>(p[2]) << 16)
           | (static_cast<uint32_t>(p[3]) << 24);
}

inline void WriteU16(std::vector<uint8_t>& dst, uint16_t value)
{
    const uint8_t bytes[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    dst.insert(dst.end(), bytes, bytes + 2);
}

inline void WriteU32(std::vector<uint8_t>& dst, uint32_t value)
{
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF),
    };
    dst.insert(dst.end(), bytes, bytes + 4);
}

inline bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
{
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
        return false;
#else
    file = std::fopen(path.string().c_str(), "rb");
    if (!file)
        return false;
#endif

    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }

    const long size = std::ftell(file);
    if (size < 0)
    {
        std::fclose(file);
        return false;
    }

    if (std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }

    bytes.resize(static_cast<size_t>(size));

    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    return read == bytes.size();
}

inline bool WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0)
        return false;
#else
    file = std::fopen(path.string().c_str(), "wb");
    if (!file)
        return false;
#endif

    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    return written == bytes.size();
}

inline bool LoadMesh(const std::filesystem::path& path, MeshData& mesh, std::string& error)
{
    mesh = MeshData{};

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes))
    {
        error = "failed to open input file";
        return false;
    }

    if (bytes.size() < sizeof(MeshFileHeader))
    {
        error = "file too small for a mesh header";
        return false;
    }

    const uint8_t* cursor = bytes.data();

    const uint32_t magic          = ReadU32(cursor);
    const uint32_t version        = ReadU32(cursor + 4);
    const uint32_t flags          = ReadU32(cursor + 8);
    const uint32_t faceCount      = ReadU32(cursor + 12);
    const uint32_t vertexCount    = ReadU32(cursor + 16);
    const uint32_t vertexStride   = ReadU32(cursor + 20);
    const uint32_t positionOffset = ReadU32(cursor + 24);
    cursor += sizeof(MeshFileHeader);

    if (magic != MESH_FILE_MAGIC)
    {
        error = "bad file magic";
        return false;
    }

    if (version != MESH_FILE_VERSION)
    {
        error = "unsupported mesh file version";
        return false;
    }

    if (faceCount == 0 || vertexCount == 0 || vertexStride == 0)
    {
        error = "mesh contains no geometry";
        return false;
    }

    const bool index32 = (flags & MESH_FLAG_INDEX_32) != 0;
    const uint64_t indexBytes = uint64_t(faceCount) * 3
                                * (index32 ? sizeof(uint32_t) : sizeof(uint16_t));
    const uint64_t attributeBytes = (flags & MESH_FLAG_HAS_ATTRIBUTES)
                                        ? uint64_t(faceCount) * sizeof(uint32_t)
                                        : 0;
    const uint64_t vertexBytes = uint64_t(vertexCount) * vertexStride;
    const uint64_t requiredBytes = uint64_t(sizeof(MeshFileHeader))
                                   + indexBytes + attributeBytes + vertexBytes;

    if (requiredBytes > bytes.size())
    {
        error = "truncated mesh file";
        return false;
    }

    mesh.indices.resize(static_cast<size_t>(faceCount) * 3);
    if (index32)
    {
        for (size_t j = 0; j < mesh.indices.size(); ++j)
        {
            mesh.indices[j] = ReadU32(cursor);
            cursor += sizeof(uint32_t);
        }
    }
    else
    {
        for (size_t j = 0; j < mesh.indices.size(); ++j)
        {
            mesh.indices[j] = ReadU16(cursor);
            cursor += sizeof(uint16_t);
        }
    }

    if (flags & MESH_FLAG_HAS_ATTRIBUTES)
    {
        mesh.attributes.resize(faceCount);
        for (uint32_t j = 0; j < faceCount; ++j)
        {
            mesh.attributes[j] = ReadU32(cursor);
            cursor += sizeof(uint32_t);
        }
    }

    mesh.vertices.assign(cursor, cursor + static_cast<size_t>(vertexBytes));
    mesh.vertexStride   = vertexStride;
    mesh.positionOffset = positionOffset;

    if (mesh.GetVertexCount() != vertexCount)
    {
        error = "vertex buffer size does not match header";
        return false;
    }

    for (uint32_t index : mesh.indices)
    {
        if (index >= vertexCount)
        {
            error = "index buffer references a vertex outside the vertex buffer";
            return false;
        }
    }

    if (mesh.HasAttributes() && mesh.attributes.size() != faceCount)
    {
        error = "attribute count does not match face count";
        return false;
    }

    if (positionOffset != UINT32_MAX
        && (static_cast<size_t>(positionOffset) + 3 * sizeof(float) > vertexStride))
    {
        error = "position channel is outside the vertex stride";
        return false;
    }

    return true;
}

inline bool SaveMesh(const std::filesystem::path& path,
                     const MeshData& mesh,
                     bool forceIndex32,
                     std::string& error)
{
    const uint32_t faceCount   = mesh.GetFaceCount();
    const uint32_t vertexCount = mesh.GetVertexCount();

    if (faceCount == 0 || vertexCount == 0 || mesh.vertexStride == 0)
    {
        error = "mesh contains no geometry";
        return false;
    }

    uint32_t maxIndex = 0;
    for (uint32_t index : mesh.indices)
    {
        if (index > maxIndex)
            maxIndex = index;
    }

    const bool index32 = forceIndex32 || (maxIndex > UINT16_MAX);

    MeshFileHeader header{};
    header.magic          = MESH_FILE_MAGIC;
    header.version        = MESH_FILE_VERSION;
    header.flags          = index32 ? MESH_FLAG_INDEX_32 : 0;
    header.faceCount      = faceCount;
    header.vertexCount    = vertexCount;
    header.vertexStride   = mesh.vertexStride;
    header.positionOffset = mesh.positionOffset;

    if (mesh.HasAttributes())
        header.flags |= MESH_FLAG_HAS_ATTRIBUTES;

    std::vector<uint8_t> bytes;
    bytes.reserve(sizeof(header) + mesh.indices.size() * sizeof(uint32_t)
                  + mesh.attributes.size() * sizeof(uint32_t) + mesh.vertices.size());

    auto pushHeaderField = [&](uint32_t value) { WriteU32(bytes, value); };

    pushHeaderField(MESH_FILE_MAGIC);
    pushHeaderField(MESH_FILE_VERSION);
    pushHeaderField(header.flags);
    pushHeaderField(header.faceCount);
    pushHeaderField(header.vertexCount);
    pushHeaderField(header.vertexStride);
    pushHeaderField(header.positionOffset);

    if (index32)
    {
        for (uint32_t index : mesh.indices)
            WriteU32(bytes, index);
    }
    else
    {
        for (uint32_t index : mesh.indices)
            WriteU16(bytes, static_cast<uint16_t>(index));
    }

    for (uint32_t attribute : mesh.attributes)
        WriteU32(bytes, attribute);

    bytes.insert(bytes.end(), mesh.vertices.begin(), mesh.vertices.end());

    if (!WriteFileBytes(path, bytes))
    {
        error = "failed to write output file";
        return false;
    }

    return true;
}

struct OptimizationReport
{
    float  acmrBefore = 0.0f;
    float  atvrBefore = 0.0f;
    float  acmrAfter  = 0.0f;
    float  atvrAfter  = 0.0f;
    size_t cacheSize  = 0;
};

OptimizationReport OptimizeMesh(MeshData& mesh,
                                const std::string& algorithm,
                                size_t cacheSize,
                                bool attributeSort);

void PrintReport(const OptimizationReport& report);
} // namespace meshopt
