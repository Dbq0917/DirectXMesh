# meshoptimizer

Cross-platform command-line tool that improves the post-transform vertex cache
hit rate of a mesh by reordering faces and vertices. Both operations are pure
permutations of the existing data:

- No vertex is created, removed, or modified, so positions, normals, UVs,
  colors and any other vertex channels are preserved byte for byte.
- Triangle topology and winding are preserved, so the rendered mesh is
  identical before and after optimization.

The tool is built on the DirectXMesh `OptimizeFaces` / `OptimizeFacesLRU` and
`OptimizeVertices` pipelines and runs on Windows, Linux, and macOS.

## Why

When a mesh is exported without cache-aware ordering, triangles that share
vertices can be far apart in the index buffer. Each shared vertex is then
refetched and re-shaded repeatedly, which shows up as a high average cache
miss ratio (ACMR) and slower vertex processing.

The optimizer applies two stages:

1. **Face reorder** - triangles are resequenced so vertices still resident in
   the post-transform FIFO cache are reused, using either Hoppe tipsification
   or the Forsyth linear-speed LRU algorithm.
2. **Vertex reorder** - the vertex buffer is laid out in first-use fetch
   order, improving pre-transform cache and memory locality.

If reordering cannot improve the input (for example, an already optimized
mesh), the original layout is kept.

## Building

```bash
cmake --preset=x64-release -DBUILD_MESHOPTIMIZER=ON
cmake --build out/build/x64-release
```

On non-Windows platforms DirectXMath and DirectX-Headers are required
(available via vcpkg or the platform package manager).

## Usage

```bash
meshoptimizer [options] <input-file> [-o <output-file>]
```

### Options

| Option | Description |
| --- | --- |
| `-a, --algorithm <hoppe|lru>` | Face reorder algorithm. `hoppe` (default) needs position data for geometric adjacency; `lru` works on indices alone. |
| `--cache-size <n>` | FIFO cache size used for the reported ACMR/ATVR statistics. Defaults to 12. |
| `--no-sort` | Do not group faces by attribute id before optimizing. |
| `--ib32` | Force 32-bit indices in the output container. |
| `--stats` | Print cache statistics and exit without writing. |
| `-o <file>` | Output path. Defaults to `<input>.opt.meshbin`. |
| `-h, --help` | Show command-line help. |

### Examples

```bash
# Hoppe optimization of an OBJ, writing an optimized .meshbin
meshoptimizer character.obj -o character.opt.meshbin

# Index-only LRU optimization (no position channel required)
meshoptimizer level.meshbin -a lru

# Measure cache behavior without producing output
meshoptimizer level.meshbin --stats
```

## Metrics

The tool reports the same metrics as `meshconvert`:

- **ACMR** (average cache miss ratio): vertex shader invocations per
  triangle. The theoretical optimum is 0.5; an unoptimized mesh is near 3.0.
- **ATVR** (average transformed vertex ratio): vertex shader invocations per
  unique vertex. The theoretical optimum is 1.0.

Both are measured against a FIFO cache of `--cache-size` entries using
`ComputeVertexCacheMissRate`.

## `.meshbin` Container

A little-endian, tightly packed binary container used for generic vertex
streams. All multi-byte fields are little-endian regardless of host byte
order.

### Header (28 bytes)

| Offset | Type | Field |
| --- | --- | --- |
| 0 | uint32 | Magic, always `0x4853454D` (`MESH`) |
| 4 | uint32 | Format version, currently `1` |
| 8 | uint32 | Flags |
| 12 | uint32 | Face count |
| 16 | uint32 | Vertex count |
| 20 | uint32 | Vertex stride in bytes |
| 24 | uint32 | Position channel offset, or `UINT32_MAX` if absent |

### Flags

| Bit | Meaning |
| --- | --- |
| `0x1` | Per-face uint32 attribute section is present |
| `0x2` | Indices are uint32; otherwise indices are uint16 |

### Sections, in order

1. **Indices** - `faceCount * 3` entries, uint16 or uint32 per the flags.
2. **Attributes** (optional) - `faceCount` uint32 attribute ids. When present,
   faces are grouped by attribute id before cache optimization, and each
   attribute subset is optimized independently.
3. **Vertices** - `vertexCount * vertexStride` raw bytes. The stream is
   treated as opaque, so any vertex layout is supported.

## Input Formats

- `.meshbin` - the container described above.
- `.obj` - WaveFront OBJ. Vertices are loaded as float3 position, float3
  normal and float2 texture coordinate (32-byte stride), and `usemtl` groups
  become face attributes.
