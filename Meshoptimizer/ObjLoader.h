//-------------------------------------------------------------------------------------
// File: ObjLoader.h
//
// Minimal cross-platform WaveFront OBJ loader for the mesh optimizer.
//
// The loader keeps the WaveFrontReader vertex layout (float3 position,
// float3 normal, float2 texture coordinate) and produces one attribute id per
// face based on the active usemtl group. It supports triangulated polygons and
// 1-based OBJ index triplets (v/vt/vn), including negative indices.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#pragma once

#include "MeshCacheOpt.h"

#include <DirectXMath.h>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace meshopt
{
    using DirectX::XMFLOAT2;
    using DirectX::XMFLOAT3;

#pragma pack(push, 1)
    struct ObjVertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 textureCoordinate;
    };
#pragma pack(pop)

    static_assert(sizeof(ObjVertex) == 32, "OBJ vertex layout size mismatch");

    struct ObjVertexKey
    {
        int64_t position;
        int64_t texcoord;
        int64_t normal;

        bool operator==(const ObjVertexKey& other) const noexcept
        {
            return position == other.position
                   && texcoord == other.texcoord
                   && normal == other.normal;
        }
    };

    struct ObjVertexKeyHash
    {
        size_t operator()(const ObjVertexKey& key) const noexcept
        {
            uint64_t hash = static_cast<uint64_t>(key.position) * 1000003u;
            hash ^= static_cast<uint64_t>(key.texcoord) * 1000033u;
            hash ^= static_cast<uint64_t>(key.normal) * 1000313u;
            return static_cast<size_t>(hash);
        }
    };

    inline ObjVertexKey ParseFaceToken(const std::string& token)
    {
        ObjVertexKey key{0, 0, 0};

        std::array<int64_t*, 3> fields{&key.position, &key.texcoord, &key.normal};
        size_t field = 0;

        size_t start = 0;
        for (size_t end = 0; end <= token.size(); ++end)
        {
            if (end != token.size() && token[end] != '/')
                continue;

            if (field >= fields.size())
                break;

            if (end > start)
            {
                try
                {
                    *fields[field] = std::stoll(token.substr(start, end - start));
                }
                catch (const std::exception&)
                {
                    *fields[field] = 0;
                }
            }

            ++field;
            start = end + 1;
        }

        return key;
    }

    inline bool LoadObjMesh(const std::filesystem::path& path, MeshData& mesh, std::string& error)
    {
        mesh = MeshData{};

        std::ifstream stream(path);
        if (!stream)
        {
            error = "failed to open OBJ file";
            return false;
        }

        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texcoords;

        std::vector<ObjVertex> vertices;
        std::unordered_map<ObjVertexKey, uint32_t, ObjVertexKeyHash> uniqueVertices;

        std::vector<uint32_t> indices;
        std::vector<uint32_t> attributes;

        uint32_t currentAttribute = 0;
        std::unordered_map<std::string, uint32_t> materialIds;

        auto resolveIndex = [](int64_t index, size_t count) -> size_t {
            if (index > 0)
                return static_cast<size_t>(index - 1);
            if (index < 0)
                return count + static_cast<size_t>(index);
            return 0;
        };

        auto addVertex = [&](const ObjVertexKey& key) -> uint32_t {
            const auto found = uniqueVertices.find(key);
            if (found != uniqueVertices.end())
                return found->second;

            const size_t positionIndex = resolveIndex(key.position, positions.size());
            if (positionIndex >= positions.size())
                throw std::runtime_error("OBJ face references an out-of-range vertex");

            ObjVertex vertex{};
            vertex.position = positions[positionIndex];

            if (key.normal != 0)
            {
                const size_t normalIndex = resolveIndex(key.normal, normals.size());
                if (normalIndex < normals.size())
                    vertex.normal = normals[normalIndex];
            }

            if (key.texcoord != 0)
            {
                const size_t texcoordIndex = resolveIndex(key.texcoord, texcoords.size());
                if (texcoordIndex < texcoords.size())
                    vertex.textureCoordinate = texcoords[texcoordIndex];
            }

            const uint32_t newIndex = static_cast<uint32_t>(vertices.size());
            if (vertices.size() >= UINT32_MAX)
                throw std::runtime_error("OBJ contains too many unique vertices");
            vertices.push_back(vertex);
            uniqueVertices.emplace(key, newIndex);
            return newIndex;
        };

        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            std::istringstream lineStream(line);
            std::string command;
            lineStream >> command;

            if (command == "v")
            {
                XMFLOAT3 value{};
                lineStream >> value.x >> value.y >> value.z;
                positions.push_back(value);
            }
            else if (command == "vn")
            {
                XMFLOAT3 value{};
                lineStream >> value.x >> value.y >> value.z;
                normals.push_back(value);
            }
            else if (command == "vt")
            {
                XMFLOAT2 value{};
                lineStream >> value.x >> value.y;
                texcoords.push_back(value);
            }
            else if (command == "usemtl")
            {
                std::string material;
                lineStream >> material;

                const auto found = materialIds.find(material);
                if (found != materialIds.end())
                {
                    currentAttribute = found->second;
                }
                else
                {
                    currentAttribute = static_cast<uint32_t>(materialIds.size());
                    materialIds.emplace(material, currentAttribute);
                }
            }
            else if (command == "f")
            {
                std::vector<ObjVertexKey> face;
                std::string token;
                while (lineStream >> token)
                    face.push_back(ParseFaceToken(token));

                if (face.size() < 3)
                    continue;

                for (size_t corner = 1; corner + 1 < face.size(); ++corner)
                {
                    indices.push_back(addVertex(face[0]));
                    indices.push_back(addVertex(face[corner]));
                    indices.push_back(addVertex(face[corner + 1]));
                    attributes.push_back(currentAttribute);
                }
            }
        }

        if (indices.empty() || vertices.empty())
        {
            error = "OBJ contains no triangle geometry";
            return false;
        }

        mesh.indices = std::move(indices);
        mesh.attributes = std::move(attributes);
        mesh.vertexStride = static_cast<uint32_t>(sizeof(ObjVertex));
        mesh.positionOffset = 0;
        mesh.vertices.resize(vertices.size() * sizeof(ObjVertex));
        std::memcpy(mesh.vertices.data(), vertices.data(), mesh.vertices.size());

        return true;
    }
} // namespace meshopt
