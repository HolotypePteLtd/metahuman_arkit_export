#!/usr/bin/env python3
"""Dump GLB structure: meshes, morph target names, animations, node tree."""
import json, struct, sys

def inspect(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, total = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF", (path, magic)
    clen, ctype = struct.unpack_from("<I4s", data, 12)
    assert ctype == b"JSON"
    gltf = json.loads(data[20:20 + clen].decode("utf-8"))
    print(f"== {path}")
    print(f"  version={ver} size={total}")
    print(f"  meshes={len(gltf.get('meshes', []))} nodes={len(gltf.get('nodes', []))}")
    for i, m in enumerate(gltf.get("meshes", [])):
        prims = m.get("primitives", [])
        names = m.get("extras", {}).get("targetNames", [])
        print(f"  mesh[{i}] '{m.get('name')}' prims={len(prims)} morphs={len(names)}")
        for p in prims[:1]:
            attrs = list(p.get("attributes", {}).keys())
            print(f"    prim attrs={attrs} morph_targets={len(p.get('targets', []))}")
        print(f"    morph names: {names}")
    anims = gltf.get("animations", [])
    print(f"  animations={len(anims)}")
    for a in anims[:3]:
        print(f"    anim '{a.get('name')}' channels={len(a.get('channels', []))}")
    # top-level scene nodes
    scene = gltf.get("scenes", [{}])[0]
    print(f"  root nodes: {scene.get('nodes', [])}")
    for n in gltf.get("nodes", [])[:10]:
        print(f"    node '{n.get('name')}' mesh={n.get('mesh')} children={len(n.get('children', []))}")

for p in sys.argv[1:]:
    inspect(p)
    print()
