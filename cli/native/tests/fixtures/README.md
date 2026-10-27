# Synthetic regression fixtures

These small DNA files contain generated triangles, joints and controls, with
no MetaHuman or other character assets. `head.dna` has two LODs, two meshes,
parent-after-child joint ordering, nonzero rest rotations, translation/rotation/
scale behavior, multiple skin influences, corrective shapes and UV seams.

`expected.json` captures the retired Python reference's evaluated neutral and
all 52 target arrays, default recipes, target order and provenance. Its hashes
identify the historical reference source; that implementation is no longer
needed to build or run tests. Raw and GUI mappings exercise the same poses,
including two subtractive recipes that deliberately yield zero targets.

Native tests compare evaluation, serialized GLB accessors, and reloaded baked
DNA against these fixed expectations. They also check all 52 individual and
three mixed poses, topology/skin retention, source roundtrips, failed-output
atomicity, Unicode paths and standalone execution.

Update the expectations only for an intentional output-contract change,
reviewing the numerical differences independently of the implementation.
