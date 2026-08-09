#!/usr/bin/env python3
"""Generate the demo OBJ models: a wavy terrain grid (terrain.obj) and a
flat-shaded octahedron gem (gem.obj). Written into data/models/."""
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "models")
os.makedirs(OUT, exist_ok=True)


def wave(x, z):
    return (0.55 * math.sin(x * 0.9) * math.cos(z * 0.75)
            + 0.28 * math.sin(x * 2.1 + z * 1.3))


def write_terrain(path, n=9, span=8.0):
    """n x n vertex grid across [-span/2, span/2]; flat-ish UVs."""
    half = span / 2.0
    verts, uvs, norms, faces = [], [], [], []
    for iz in range(n):
        for ix in range(n):
            x = -half + span * ix / (n - 1)
            z = -half + span * iz / (n - 1)
            y = wave(x, z)
            verts.append((x, y, z))
            uvs.append((ix / (n - 1), iz / (n - 1)))
            # finite-difference normal
            dx = span / (n - 1)
            hx1 = wave(x + dx, z)
            hx0 = wave(x - dx, z)
            hz1 = wave(x, z + dx)
            hz0 = wave(x, z - dx)
            nx = -(hx1 - hx0) / (2 * dx)
            nz = -(hz1 - hz0) / (2 * dx)
            ny = 1.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            norms.append((nx * inv, ny * inv, nz * inv))
    for iz in range(n - 1):
        for ix in range(n - 1):
            a = iz * n + ix
            b = a + 1
            c = a + n
            d = c + 1
            # CCW when viewed from +y
            faces.append((a, c, d))
            faces.append((a, d, b))
    with open(path, "w") as f:
        f.write("# NULL SECTOR // terrain.obj (generated: wavy PBR test grid)\n")
        f.write("o terrain\n")
        for v in verts:
            f.write("v %.4f %.4f %.4f\n" % v)
        for t in uvs:
            f.write("vt %.4f %.4f\n" % t)
        for nrm in norms:
            f.write("vn %.4f %.4f %.4f\n" % nrm)
        f.write("s 1\n")
        for a, b, c in faces:
            f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a + 1, a + 1, a + 1,
                                                        b + 1, b + 1, b + 1,
                                                        c + 1, c + 1, c + 1))
    print("wrote", path, len(verts), "verts,", len(faces), "faces")


def write_gem(path):
    """Octahedron with flat per-face normals (hand-normalized)."""
    verts = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    # faces: (upper +y ring), (lower -y ring) - each face points outward
    faces = [
        (0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4),   # +z cap ring
        (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5),   # -z cap ring
    ]
    with open(path, "w") as f:
        f.write("# NULL SECTOR // gem.obj (octahedron, flat normals)\n")
        f.write("o gem\n")
        for v in verts:
            f.write("v %.4f %.4f %.4f\n" % v)
        for t in [(0, 0), (1, 0), (0.5, 1)]:
            f.write("vt %.4f %.4f\n" % t)
        for a, b, c in faces:
            v1 = verts[a]
            v2 = verts[b]
            v3 = verts[c]
            # flat normal = normalize(v1 + v2 + v3) (symmetric octahedron)
            nx = v1[0] + v2[0] + v3[0]
            ny = v1[1] + v2[1] + v3[1]
            nz = v1[2] + v2[2] + v3[2]
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            f.write("vn %.4f %.4f %.4f\n" % (nx * inv, ny * inv, nz * inv))
        f.write("s off\n")
        for i, (a, b, c) in enumerate(faces):
            u = i % 3
            u1 = (u + 1) % 3
            f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a + 1, u + 1, i + 1,
                                                        b + 1, u1 + 1, i + 1,
                                                        c + 1, u + 2, i + 1))
    print("wrote", path)


write_terrain(os.path.join(OUT, "terrain.obj"))
write_gem(os.path.join(OUT, "gem.obj"))
