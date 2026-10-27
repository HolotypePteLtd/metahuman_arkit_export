#include "baker.h"
#include <algorithm>

namespace arkit {
namespace {
void require(bool condition, const std::string &field) {
  if (!condition)
    throw std::runtime_error("Invalid DNA " + field);
}
template <class View>
void indices(View values, std::size_t count, const std::string &field,
             std::size_t minimum = 0) {
  for (auto value : values)
    require(value >= minimum && value < count, field + " index");
}
template <class View> void finite(View values, const std::string &field) {
  for (auto value : values)
    require(std::isfinite(value), field + " value");
}
void lods(dna::ConstArrayView<std::uint16_t> values, std::size_t count,
          std::size_t lod_count, const std::string &field) {
  if (values.size() == 0 && count == 0)
    return;
  require(values.size() == lod_count, field + " LOD count");
  std::size_t previous = count;
  for (auto value : values) {
    require(value <= previous, field + " LOD bound");
    previous = value;
  }
}
void conditional(dna::ConstArrayView<std::uint16_t> input,
                 dna::ConstArrayView<std::uint16_t> output,
                 dna::ConstArrayView<float> from, dna::ConstArrayView<float> to,
                 dna::ConstArrayView<float> slope,
                 dna::ConstArrayView<float> cut, std::size_t inputs,
                 std::size_t outputs, const std::string &field) {
  auto count = input.size();
  require(count <= std::numeric_limits<std::uint16_t>::max() &&
              output.size() == count && from.size() == count &&
              to.size() == count && slope.size() == count &&
              cut.size() == count,
          field + " array lengths");
  indices(input, inputs, field + " input");
  indices(output, outputs, field + " output");
  finite(from, field);
  finite(to, field);
  finite(slope, field);
  finite(cut, field);
}
} // namespace

// The stream reader's successful status does not validate the cross-references
// that RigLogic uses as unchecked native array indices.
void validate_behavior(const dna::Reader &r) {
  const auto joints = std::size_t(r.getJointCount());
  const auto raw = std::size_t(r.getRawControlCount());
  const auto psd_end = raw + r.getPSDCount();
  const auto controls =
      psd_end + r.getMLControlCount() + r.getRBFPoseControlCount();
  const auto limit = std::numeric_limits<std::uint16_t>::max();
  require(r.getLODCount() != 0, "LOD count");
  // RigLogic stores total control and quaternion joint attribute indices in
  // uint16_t, even though each constituent count can separately fit that type.
  require(controls <= limit && joints * 10 <= limit, "aggregate counts");
  for (auto values :
       {r.getNeutralJointTranslationXs(), r.getNeutralJointTranslationYs(),
        r.getNeutralJointTranslationZs(), r.getNeutralJointRotationXs(),
        r.getNeutralJointRotationYs(), r.getNeutralJointRotationZs()}) {
    require(values.size() == joints, "neutral joint array lengths");
    finite(values, "neutral joint");
  }
  conditional(r.getGUIToRawInputIndices(), r.getGUIToRawOutputIndices(),
              r.getGUIToRawFromValues(), r.getGUIToRawToValues(),
              r.getGUIToRawSlopeValues(), r.getGUIToRawCutValues(),
              r.getGUIControlCount(), raw, "GUI mapping");
  auto rows = r.getPSDRowIndices(), columns = r.getPSDColumnIndices();
  auto values = r.getPSDValues();
  require(rows.size() == columns.size() && rows.size() == values.size(),
          "PSD array lengths");
  indices(rows, psd_end, "PSD output", raw);
  indices(columns, controls, "PSD input");
  finite(values, "PSD");
  for (std::uint16_t g = 0; g < r.getJointGroupCount(); ++g) {
    const auto field = "joint group " + std::to_string(g);
    auto input = r.getJointGroupInputIndices(g);
    auto output = r.getJointGroupOutputIndices(g);
    auto matrix = r.getJointGroupValues(g);
    require(input.size() <= limit && output.size() <= limit &&
                matrix.size() == input.size() * output.size(),
            field + " matrix dimensions");
    indices(input, controls, field + " input");
    indices(output, joints * 9, field + " output");
    indices(r.getJointGroupJointIndices(g), joints, field + " joint");
    lods(r.getJointGroupLODs(g), output.size(), r.getLODCount(), field);
    finite(matrix, field);
  }
  auto blend_in = r.getBlendShapeChannelInputIndices();
  auto blend_out = r.getBlendShapeChannelOutputIndices();
  require(blend_in.size() == blend_out.size(), "blendshape array lengths");
  indices(blend_in, controls, "blendshape input");
  indices(blend_out, r.getBlendShapeChannelCount(), "blendshape output");
  lods(r.getBlendShapeChannelLODs(), blend_in.size(), r.getLODCount(),
       "blendshape");
  conditional(r.getAnimatedMapInputIndices(), r.getAnimatedMapOutputIndices(),
              r.getAnimatedMapFromValues(), r.getAnimatedMapToValues(),
              r.getAnimatedMapSlopeValues(), r.getAnimatedMapCutValues(),
              controls, r.getAnimatedMapCount(), "animated map");
  lods(r.getAnimatedMapLODs(), r.getAnimatedMapInputIndices().size(),
       r.getLODCount(), "animated map");
  for (std::uint16_t l = 0; l < r.getLODCount(); ++l) {
    indices(r.getJointIndicesForLOD(l), joints, "LOD joint");
    indices(r.getJointVariableAttributeIndices(l), joints * 9,
            "joint attribute");
    indices(r.getMeshIndicesForLOD(l), r.getMeshCount(), "LOD mesh");
    indices(r.getBlendShapeChannelIndicesForLOD(l),
            r.getBlendShapeChannelCount(), "LOD blendshape");
    indices(r.getAnimatedMapIndicesForLOD(l), r.getAnimatedMapCount(),
            "LOD animated map");
  }
  // These indices are also consumed by ControlsFactory during construction,
  // before any optional RBF or twist/swing evaluation takes place.
  for (std::uint16_t s = 0; s < r.getRBFSolverCount(); ++s)
    indices(r.getRBFSolverRawControlIndices(s), raw, "RBF raw control");
  for (std::uint16_t t = 0; t < r.getTwistCount(); ++t) {
    indices(r.getTwistInputControlIndices(t), raw, "twist raw control");
    indices(r.getTwistOutputJointIndices(t), joints, "twist joint");
  }
  for (std::uint16_t s = 0; s < r.getSwingCount(); ++s) {
    indices(r.getSwingInputControlIndices(s), raw, "swing raw control");
    indices(r.getSwingOutputJointIndices(s), joints, "swing joint");
  }
}
} // namespace arkit
