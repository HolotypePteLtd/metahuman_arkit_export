"""Write and verify a static-skeleton ARKit DNA from evaluated LOD0 poses."""
from pathlib import Path
import tempfile

import numpy as np


def control_names(names):
    return ["CTRL_expressions." + name[0].lower() + name[1:] for name in names]


def verify_baked_dna(path, names, meshes, neutral, targets, dna, riglogic):
    from bake_arkit_dna import NativeRig, read_mesh

    rig = NativeRig(path, dna, riglogic)
    reader = rig.reader
    expected_controls = control_names(names)
    channels = [reader.getBlendShapeChannelName(i)
                for i in range(reader.getBlendShapeChannelCount())]
    if rig.raw_names != expected_controls or channels != list(names):
        raise ValueError("Baked DNA controls or channels do not match the ARKit contract")
    actual_meshes = [read_mesh(reader, i) for i in reader.getMeshIndicesForLOD(0)]
    if [mesh.name for mesh in actual_meshes] != [mesh.name for mesh in meshes]:
        raise ValueError("Baked DNA mesh order changed")
    maximum = 0.0

    def check(weights):
        nonlocal maximum
        controls = {name: float(value) for name, value in zip(expected_controls, weights, strict=True)
                    if value != 0}
        actual = rig.evaluate(actual_meshes, controls, "raw")
        for index, positions in enumerate(actual):
            expected = neutral[index].copy()
            for weight, target in zip(weights, targets, strict=True):
                expected += weight * target[index]
            if positions.shape != expected.shape or not np.all(np.isfinite(positions)):
                raise ValueError("Baked DNA produced invalid vertex positions")
            error = float(np.linalg.norm(positions - expected, axis=1).max())
            maximum = max(maximum, error)
            if error > 2e-6:
                raise ValueError(f"Baked DNA differs from baked geometry by {error:.9g} metres")

    check(np.zeros(len(names)))
    for weights in np.eye(len(names)):
        check(weights)
    rng = np.random.default_rng(0)
    for weights in rng.random((3, len(names))):
        check(weights)
    return {"verified": True, "poses": len(names) + 4,
            "max_error_m": maximum, "tolerance_m": 2e-6}


def write_baked_dna(path, rig, indices, meshes, neutral, targets, names, dna, riglogic):
    """Retain neutral joints/skin/topology; replace live behavior with morphs.

    The native reader has already converted the source to right/up/back axes.
    Evaluated positions are in metres; DNA retains its original linear unit.
    No expression moves the skeleton: joint movement is baked into the deltas.
    """
    reader = rig.reader
    count = len(names)
    controls = control_names(names)
    path = Path(path)
    with tempfile.TemporaryDirectory(prefix=".arkit-dna-", dir=path.parent) as directory:
        temporary = Path(directory) / "baked.dna"
        stream = dna.FileStream(str(temporary), dna.AccessMode_Write, dna.OpenMode_Binary)
        writer = dna.BinaryStreamWriter(stream)
        # This overload copies native data rather than reconstructing adapted
        # fields through Python. Exclude the source solvers and unknown layers,
        # whose indices refer to the old raw-control/channel tables.
        writer.setFrom(reader, dna.DataLayer_Geometry, dna.UnknownLayerPolicy_Ignore)
        writer.clearGUIControlNames()
        writer.clearRawControlNames()
        writer.clearBlendShapeChannelNames()
        writer.clearBlendShapeChannelIndices()
        writer.clearLODBlendShapeChannelMappings()
        writer.clearAnimatedMapNames()
        writer.clearAnimatedMapIndices()
        writer.clearLODAnimatedMapMappings()
        writer.clearMeshNames()
        writer.clearMeshIndices()
        writer.clearLODMeshMappings()
        writer.clearMeshBlendShapeChannelMappings()
        writer.clearMeshes()
        writer.setLODCount(1)
        # The source reader is restricted to LOD0, including its joint mapping.
        writer.setJointRowCount(reader.getJointCount() * 9)
        writer.setJointColumnCount(count)
        writer.setPSDCount(0)
        for index, name in enumerate(names):
            writer.setGUIControlName(index, name)
            writer.setRawControlName(index, controls[index])
            writer.setBlendShapeChannelName(index, name)
        identity = list(range(count))
        writer.setGUIToRawInputIndices(identity)
        writer.setGUIToRawOutputIndices(identity)
        writer.setGUIToRawFromValues([0.0] * count)
        writer.setGUIToRawToValues([1.0] * count)
        writer.setGUIToRawSlopeValues([1.0] * count)
        writer.setGUIToRawCutValues([0.0] * count)
        writer.setBlendShapeChannelIndices(0, identity)
        writer.setLODBlendShapeChannelMapping(0, 0)
        writer.setBlendShapeChannelLODs([count])
        writer.setBlendShapeChannelInputIndices(identity)
        writer.setBlendShapeChannelOutputIndices(identity)
        writer.setAnimatedMapIndices(0, [])
        writer.setLODAnimatedMapMapping(0, 0)
        writer.setAnimatedMapLODs([0])
        writer.setMeshIndices(0, list(range(len(meshes))))
        writer.setLODMeshMapping(0, 0)
        for index, (source, mesh) in enumerate(zip(indices, meshes, strict=True)):
            writer.setMeshName(index, mesh.name)
            writer.setVertexPositions(index, (neutral[index] / rig.scale).tolist())
            writer.setVertexTextureCoordinates(index, [list(row) for row in zip(
                reader.getVertexTextureCoordinateUs(source), reader.getVertexTextureCoordinateVs(source))])
            writer.setVertexNormals(index, [list(row) for row in zip(
                reader.getVertexNormalXs(source), reader.getVertexNormalYs(source),
                reader.getVertexNormalZs(source))])
            writer.setVertexLayouts(index, [list(row) for row in zip(
                reader.getVertexLayoutPositionIndices(source),
                reader.getVertexLayoutTextureCoordinateIndices(source),
                reader.getVertexLayoutNormalIndices(source))])
            for face in range(reader.getFaceCount(source)):
                writer.setFaceVertexLayoutIndices(index, face, reader.getFaceVertexLayoutIndices(source, face))
            writer.setMaximumInfluencePerVertex(index, reader.getMaximumInfluencePerVertex(source))
            for vertex in range(len(mesh.positions)):
                writer.setSkinWeightsValues(index, vertex, reader.getSkinWeightsValues(source, vertex))
                writer.setSkinWeightsJointIndices(index, vertex, reader.getSkinWeightsJointIndices(source, vertex))
            for channel, target in enumerate(targets):
                deltas = target[index] / rig.scale
                if not np.all(np.isfinite(deltas)):
                    raise ValueError("Non-finite values in baked DNA deltas")
                vertices = np.flatnonzero(np.any(deltas != 0, axis=1))
                # Keep a named target even on parts where this channel is zero.
                writer.setBlendShapeChannelIndex(index, channel, channel)
                writer.setBlendShapeTargetVertexIndices(index, channel, vertices.tolist())
                writer.setBlendShapeTargetDeltas(index, channel, deltas[vertices].tolist())
                writer.setMeshBlendShapeChannelMapping(index * count + channel, index, channel)
        writer.write()
        if not dna.Status.isOk():
            raise ValueError("Baked DNA write failed: " + dna.Status.get().message)
        verification = verify_baked_dna(temporary, names, meshes, neutral, targets, dna, riglogic)
        temporary.replace(path)
    return verification
