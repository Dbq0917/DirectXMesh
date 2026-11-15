//-------------------------------------------------------------------------------------
// File: MeshCacheOpt.cpp
//
// Vertex cache optimization pipeline built on DirectXMesh.
//
// Two stages are applied:
//   1. Face reordering to improve post-transform vertex cache reuse
//      (Hoppe tipsification or Forsyth linear-speed LRU).
//   2. Vertex reordering so the vertex buffer is laid out in fetch order,
//      improving pre-transform cache / memory locality.
//
// Both stages are pure permutations, so the rendered mesh is unchanged.
//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//-------------------------------------------------------------------------------------

#include "MeshCacheOpt.h"

#include "DirectXMesh.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace meshopt
{
namespace
{
    constexpr float kAdjacencyEpsilon = 1e-5f;

    void ComputeStats(const MeshData& mesh, size_t cacheSize, float& acmr, float& atvr)
    {
        ComputeVertexCacheMissRate(mesh.indices.data(),
                                   mesh.GetFaceCount(),
                                   mesh.GetVertexCount(),
                                   cacheSize,
                                   acmr,
                                   atvr);
    }

    bool CopyPositions(const MeshData& mesh, std::vector<XMFLOAT3>& positions)
    {
        if (mesh.positionOffset == UINT32_MAX)
            return false;

        if (static_cast<size_t>(mesh.positionOffset) + sizeof(XMFLOAT3) > mesh.vertexStride)
            return false;

        const uint32_t vertexCount = mesh.GetVertexCount();
        positions.resize(vertexCount);

        const uint8_t* source = mesh.vertices.data() + mesh.positionOffset;
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            std::memcpy(&positions[vertex], source, sizeof(XMFLOAT3));
            source += mesh.vertexStride;
        }

        return true;
    }

    void ApplyFaceRemap(const std::vector<uint32_t>& faceRemap, MeshData& mesh)
    {
        const size_t faceCount = mesh.GetFaceCount();

        std::vector<uint32_t> reorderedIndices(faceCount * 3);
        if (FAILED(ReorderIB(mesh.indices.data(), faceCount, faceRemap.data(), reorderedIndices.data())))
            throw std::runtime_error("ReorderIB failed");
        mesh.indices.swap(reorderedIndices);
        // Both OptimizeFacesEx and OptimizeFacesLRUEx optimize faces within
        // each attribute subset in place, so the attributes stay aligned.
    }

    void ReorderFacesLRU(MeshData& mesh)
    {
        const size_t faceCount = mesh.GetFaceCount();
        const size_t vertexCount = mesh.GetVertexCount();

        std::vector<uint32_t> faceRemap(faceCount);

        HRESULT hr = E_FAIL;
        if (mesh.HasAttributes())
        {
            hr = OptimizeFacesLRUEx(mesh.indices.data(),
                                    faceCount,
                                    vertexCount,
                                    mesh.attributes.data(),
                                    faceRemap.data());
        }
        else
        {
            hr = OptimizeFacesLRU(mesh.indices.data(),
                                  faceCount,
                                  vertexCount,
                                  faceRemap.data());
        }

        if (FAILED(hr))
            throw std::runtime_error("OptimizeFacesLRU failed");

        ApplyFaceRemap(faceRemap, mesh);
    }

    void ReorderFacesHoppe(MeshData& mesh)
    {
        const size_t faceCount = mesh.GetFaceCount();
        const size_t vertexCount = mesh.GetVertexCount();

        std::vector<XMFLOAT3> positions;
        if (!CopyPositions(mesh, positions))
            throw std::runtime_error("Hoppe optimization needs position data; use --algorithm lru");

        std::vector<uint32_t> adjacency(faceCount * 3);
        if (FAILED(GenerateAdjacencyAndPointReps(mesh.indices.data(),
                                                 faceCount,
                                                 positions.data(),
                                                 vertexCount,
                                                 kAdjacencyEpsilon,
                                                 nullptr,
                                                 adjacency.data())))
        {
            throw std::runtime_error("GenerateAdjacencyAndPointReps failed");
        }

        std::vector<uint32_t> faceRemap(faceCount);

        HRESULT hr = E_FAIL;
        if (mesh.HasAttributes())
        {
            hr = OptimizeFacesEx(mesh.indices.data(),
                                 faceCount,
                                 vertexCount,
                                 adjacency.data(),
                                 mesh.attributes.data(),
                                 faceRemap.data());
        }
        else
        {
            hr = OptimizeFaces(mesh.indices.data(),
                               faceCount,
                               vertexCount,
                               adjacency.data(),
                               faceRemap.data());
        }

        if (FAILED(hr))
            throw std::runtime_error("OptimizeFaces failed");

        ApplyFaceRemap(faceRemap, mesh);
    }

    void ReorderVertices(MeshData& mesh)
    {
        const size_t faceCount = mesh.GetFaceCount();
        const size_t vertexCount = mesh.GetVertexCount();

        std::vector<uint32_t> vertexRemap(vertexCount);
        size_t trailingUnused = 0;

        if (FAILED(OptimizeVertices(mesh.indices.data(),
                                    faceCount,
                                    vertexCount,
                                    vertexRemap.data(),
                                    &trailingUnused)))
        {
            throw std::runtime_error("OptimizeVertices failed");
        }

        std::vector<uint32_t> remappedIndices(faceCount * 3);
        if (FAILED(FinalizeIB(mesh.indices.data(),
                              faceCount,
                              vertexRemap.data(),
                              vertexCount,
                              remappedIndices.data())))
        {
            throw std::runtime_error("FinalizeIB failed");
        }
        mesh.indices.swap(remappedIndices);

        if (FAILED(FinalizeVB(mesh.vertices.data(),
                              mesh.vertexStride,
                              vertexCount,
                              vertexRemap.data())))
        {
            throw std::runtime_error("FinalizeVB failed");
        }
    }

    void SortAttributes(MeshData& mesh)
    {
        if (!mesh.HasAttributes())
            return;

        const size_t faceCount = mesh.GetFaceCount();

        std::vector<uint32_t> faceRemap(faceCount);
        if (FAILED(AttributeSort(faceCount, mesh.attributes.data(), faceRemap.data())))
            throw std::runtime_error("AttributeSort failed");

        // AttributeSort reorders mesh.attributes in place and faceRemap[j]
        // gives the source face for output slot j, so only the index buffer
        // needs the permutation applied.
        std::vector<uint32_t> reorderedIndices(faceCount * 3);
        if (FAILED(ReorderIB(mesh.indices.data(), faceCount, faceRemap.data(), reorderedIndices.data())))
            throw std::runtime_error("ReorderIB failed");
        mesh.indices.swap(reorderedIndices);
    }
} // namespace

OptimizationReport OptimizeMesh(MeshData& mesh,
                                const std::string& algorithm,
                                size_t cacheSize,
                                bool attributeSort)
{
    OptimizationReport report{};
    report.cacheSize = cacheSize;
    ComputeStats(mesh, cacheSize, report.acmrBefore, report.atvrBefore);

    // Work on a copy so the input is kept when reordering cannot improve it
    // (for example when the mesh was already optimized).
    MeshData candidate = mesh;

    if (attributeSort)
    {
        SortAttributes(candidate);
    }

    if (algorithm == "lru")
    {
        ReorderFacesLRU(candidate);
    }
    else if (algorithm == "hoppe")
    {
        ReorderFacesHoppe(candidate);
    }
    else
    {
        throw std::runtime_error("unknown algorithm: " + algorithm);
    }

    ReorderVertices(candidate);

    ComputeStats(candidate, cacheSize, report.acmrAfter, report.atvrAfter);

    // Only adopt the reordering when it strictly reduces cache misses. A
    // permutation never changes the rendered geometry, so keeping the original
    // layout when there is no gain is always safe.
    if (report.acmrAfter < report.acmrBefore)
    {
        mesh = std::move(candidate);
    }
    else
    {
        report.acmrAfter = report.acmrBefore;
        report.atvrAfter = report.atvrBefore;
    }

    return report;
}

void PrintReport(const OptimizationReport& report)
{
    const float hitBefore = 1.0f - report.acmrBefore / 3.0f;
    const float hitAfter  = 1.0f - report.acmrAfter / 3.0f;
    const float reduction = report.acmrBefore > 0.0f
                                ? (1.0f - report.acmrAfter / report.acmrBefore) * 100.0f
                                : 0.0f;

    std::printf("Vertex cache model: FIFO, %zu entries\n", report.cacheSize);
    std::printf("  before: ACMR %.4f, ATVR %.4f, hit rate %.2f%%\n",
                report.acmrBefore,
                report.atvrBefore,
                hitBefore * 100.0f);
    std::printf("  after:  ACMR %.4f, ATVR %.4f, hit rate %.2f%%\n",
                report.acmrAfter,
                report.atvrAfter,
                hitAfter * 100.0f);
    std::printf("  ACMR reduction: %.2f%%\n", reduction);
}
} // namespace meshopt
