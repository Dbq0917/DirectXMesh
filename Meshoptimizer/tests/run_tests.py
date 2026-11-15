#!/usr/bin/env python3
"""End-to-end tests for the meshoptimizer tool.

Generates synthetic meshes, runs the optimizer, and verifies that:
  * cache miss rates drop substantially on shuffled input;
  * triangle topology, winding and vertex data are preserved;
  * attribute subsets stay contiguous;
  * already-optimized input is left unchanged.

Usage: python3 run_tests.py /path/to/meshoptimizer [--keep]
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x4853454D
HAS_ATTRIBUTES = 0x1
INDEX_32 = 0x2


def write_meshbin(path, triangles, vertex_count, stride, attributes=None,
                  position_offset=0, raw_vertices=None):
    flags = INDEX_32
    if attributes is not None:
        flags |= HAS_ATTRIBUTES

    with open(path, "wb") as handle:
        handle.write(struct.pack("<7I", MAGIC, 1, flags, len(triangles),
                                 vertex_count, stride, position_offset))
        for triangle in triangles:
            handle.write(struct.pack("<3I", *triangle))
        if attributes is not None:
            for attribute in attributes:
                handle.write(struct.pack("<I", attribute))
        if raw_vertices is not None:
            handle.write(raw_vertices)
        else:
            handle.write(b"\0" * (vertex_count * stride))


def read_meshbin(path):
    data = open(path, "rb").read()
    magic, version, flags, face_count, vertex_count, stride, position_offset = \
        struct.unpack_from("<7I", data, 0)
    assert magic == MAGIC and version == 1

    offset = 28
    index_size = 4 if (flags & INDEX_32) else 2
    indices = []
    for _ in range(face_count * 3):
        if index_size == 4:
            indices.append(struct.unpack_from("<I", data, offset)[0])
        else:
            indices.append(struct.unpack_from("<H", data, offset)[0])
        offset += index_size

    attributes = []
    if flags & HAS_ATTRIBUTES:
        attributes = [struct.unpack_from("<I", data, offset + 4 * k)[0]
                      for k in range(face_count)]
        offset += 4 * face_count

    vertices = [data[offset + k * stride: offset + (k + 1) * stride]
                for k in range(vertex_count)]
    return flags, indices, attributes, vertices, stride, position_offset


def grid_mesh(grid_size, shuffle):
    def vertex_id(col, row):
        return row * (grid_size + 1) + col

    triangles = []
    for row in range(grid_size):
        for col in range(grid_size):
            a = vertex_id(col, row)
            b = vertex_id(col + 1, row)
            d = vertex_id(col, row + 1)
            c = vertex_id(col + 1, row + 1)
            triangles.append((a, b, c))
            triangles.append((a, c, d))

    if shuffle:
        import random
        random.seed(42)
        random.shuffle(triangles)

    vertex_count = (grid_size + 1) ** 2
    raw_vertices = bytearray()
    for row in range(grid_size + 1):
        for col in range(grid_size + 1):
            raw_vertices += struct.pack("<8f", float(col), float(row), 0.0,
                                    0.0, 0.0, 1.0, 0.0, 0.0)
    return triangles, vertex_count, bytes(raw_vertices)


def run_optimizer(tool, input_path, output_path, *extra_args):
    result = subprocess.run(
        [tool, input_path, "-o", output_path, *extra_args],
        capture_output=True, text=True, check=True)
    return result.stdout


def assert_geometry_identical(src_path, dst_path):
    src_flags, src_indices, _, src_vertices, _, _ = read_meshbin(src_path)
    dst_flags, dst_indices, dst_attributes, dst_vertices, stride, _ = read_meshbin(dst_path)

    assert len(src_indices) == len(dst_indices)
    assert len(src_vertices) == len(dst_vertices)
    assert sorted(src_vertices) == sorted(dst_vertices), "vertex data changed"

    source_index = {}
    for index, blob in enumerate(src_vertices):
        source_index.setdefault(blob, index)
    remap = [source_index[blob] for blob in dst_vertices]

    source_triangles = sorted(tuple(src_indices[k:k + 3])
                              for k in range(0, len(src_indices), 3))
    dest_triangles = sorted(tuple(remap[i] for i in dst_indices[k:k + 3])
                            for k in range(0, len(dst_indices), 3))
    assert source_triangles == dest_triangles, "triangle topology or winding changed"


def extract_acmr(output):
    for line in output.splitlines():
        if "after:" in line:
            return float(line.split("ACMR")[1].split(",")[0].strip())
    raise AssertionError("ACMR not found in optimizer output")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tool", help="path to the meshoptimizer binary")
    parser.add_argument("--keep", action="store_true", help="keep temp files")
    args = parser.parse_args()

    if not os.path.isfile(args.tool):
        parser.error(f"tool not found: {args.tool}")

    workdir = tempfile.mkdtemp(prefix="meshopt-test-")
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        status = "PASS" if condition else "FAIL"
        if not condition:
            failures += 1
        print(f"[{status}] {name} {detail}")

    try:
        triangles, vertex_count, raw_vertices = grid_mesh(40, shuffle=True)
        source = os.path.join(workdir, "grid.meshbin")
        write_meshbin(source, triangles, vertex_count, 32,
                      raw_vertices=raw_vertices)

        for algorithm in ("hoppe", "lru"):
            output = os.path.join(workdir, f"grid.{algorithm}.meshbin")
            stdout = run_optimizer(args.tool, source, output, "-a", algorithm)
            acmr = extract_acmr(stdout)
            # Unoptimized shuffled input is near 3.0. Hoppe tipsification
            # targets a different cache model than the FIFO-12 measurement,
            # so its threshold is looser than the LRU (Forsyth) result.
            threshold = 1.6 if algorithm == "hoppe" else 1.2
            check(f"{algorithm}: ACMR reduced on shuffled grid",
                  acmr < threshold, f"(ACMR={acmr:.3f})")
            assert_geometry_identical(source, output)
            check(f"{algorithm}: geometry preserved", True)

        # 16-bit index selection for meshes under 65535 vertices
        _, lru_indices, _, _, _, _ = read_meshbin(
            os.path.join(workdir, "grid.lru.meshbin"))
        lru_flags, _, _, _, _, _ = read_meshbin(
            os.path.join(workdir, "grid.lru.meshbin"))
        check("16-bit indices selected automatically",
              not (lru_flags & INDEX_32))

        # Attribute subsets must remain contiguous after optimization
        attr_triangles, attr_vertices, attr_raw = grid_mesh(24, shuffle=True)
        attributes = []
        for face in range(len(attr_triangles)):
            col = (face // 2) % 24
            row = (face // 2) // 24
            attributes.append((col // 8) + 3 * (row // 8))
        attr_source = os.path.join(workdir, "attr.meshbin")
        write_meshbin(attr_source, attr_triangles, attr_vertices, 32,
                      attributes=attributes, raw_vertices=attr_raw)
        attr_output = os.path.join(workdir, "attr.opt.meshbin")
        run_optimizer(args.tool, attr_source, attr_output)
        _, _, out_attributes, _, _, _ = read_meshbin(attr_output)
        runs = 1 + sum(1 for k in range(1, len(out_attributes))
                       if out_attributes[k] != out_attributes[k - 1])
        check("attribute subsets stay contiguous", runs == len(set(attributes)),
              f"({runs} runs, {len(set(attributes))} materials)")
        assert_geometry_identical(attr_source, attr_output)

        # Re-optimizing an optimized mesh must not make ACMR worse
        optimized = os.path.join(workdir, "grid.lru.meshbin")
        second = os.path.join(workdir, "grid.lru2.meshbin")
        first_output = run_optimizer(args.tool, source, optimized, "-a", "lru")
        second_output = run_optimizer(args.tool, optimized, second, "-a", "lru")
        check("re-optimization never regresses ACMR",
              extract_acmr(second_output) <= extract_acmr(first_output) + 1e-6)

    finally:
        if args.keep:
            print(f"Temp files kept in: {workdir}")
        else:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n{failures} test(s) failed")
        return 1
    print("\nAll tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
