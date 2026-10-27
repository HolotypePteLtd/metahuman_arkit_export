#include "baker.h"
#include <cstring>
#include <fstream>
namespace arkit {
void write_fixture(const fs::path &path, bool body) {
  auto stream = rl4::makeScoped<rl4::FileStream>(
      path.u8string().c_str(), rl4::FileStream::AccessMode::Write,
      rl4::FileStream::OpenMode::Binary);
  auto w = rl4::makeScoped<dna::BinaryStreamWriter>(stream.get());
  w->setName("Synthetic ARKit release fixture");
  w->setLODCount(1);
  w->setDBMaxLOD(0);
  w->setTranslationUnit(dna::TranslationUnit::cm);
  w->setRotationUnit(dna::RotationUnit::degrees);
  w->setCoordinateSystem(
      {dna::Direction::left, dna::Direction::up, dna::Direction::front});
  const std::uint16_t zero = 0, one = 1;
  const float fzero = 0, fone = 1;
  w->setGUIControlName(0, "CTRL_test.ty");
  w->setRawControlName(0, "CTRL_expressions.test");
  w->setGUIToRawInputIndices(&zero, 1);
  w->setGUIToRawOutputIndices(&zero, 1);
  w->setGUIToRawFromValues(&fzero, 1);
  w->setGUIToRawToValues(&fone, 1);
  w->setGUIToRawSlopeValues(&fone, 1);
  w->setGUIToRawCutValues(&fzero, 1);
  w->setPSDCount(0);
  w->setJointName(0, "root");
  w->setJointHierarchy(&zero, 1);
  w->setJointIndices(0, &zero, 1);
  w->setLODJointMapping(0, 0);
  dna::Vector3 origin{0, 0, 0};
  w->setNeutralJointTranslations(&origin, 1);
  w->setNeutralJointRotations(&origin, 1);
  w->setJointRowCount(9);
  w->setJointColumnCount(1);
  w->setJointGroupLODs(0, &one, 1);
  w->setJointGroupInputIndices(0, &zero, 1);
  w->setJointGroupOutputIndices(0, &zero, 1);
  w->setJointGroupValues(0, &fone, 1);
  w->setJointGroupJointIndices(0, &zero, 1);
  w->setBlendShapeChannelName(0, "test_corrective");
  w->setBlendShapeChannelIndices(0, &zero, 1);
  w->setLODBlendShapeChannelMapping(0, 0);
  w->setBlendShapeChannelLODs(&one, 1);
  w->setBlendShapeChannelInputIndices(&zero, 1);
  w->setBlendShapeChannelOutputIndices(&zero, 1);
  w->setMeshName(0, body ? "body_lod0_mesh" : "head_lod0_mesh");
  w->setMeshIndices(0, &zero, 1);
  w->setLODMeshMapping(0, 0);
  w->setMeshBlendShapeChannelMapping(0, 0, 0);
  float height = body ? 0.f : 150.f;
  const dna::Position p[] = {
      {0, height, 0}, {10, height, 0}, {0, height + 10, 0}};
  const dna::TextureCoordinate uv[] = {{0, 0}, {1, 0}, {0, 1}};
  const dna::Normal normal[] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  const dna::VertexLayout layout[] = {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}};
  const std::uint32_t corners[] = {0, 1, 2};
  w->setVertexPositions(0, p, 3);
  w->setVertexTextureCoordinates(0, uv, 3);
  w->setVertexNormals(0, normal, 3);
  w->setVertexLayouts(0, layout, 3);
  w->setFaceVertexLayoutIndices(0, 0, corners, 3);
  w->setMaximumInfluencePerVertex(0, 1);
  for (std::uint32_t v = 0; v < 3; ++v) {
    w->setSkinWeightsValues(0, v, &fone, 1);
    w->setSkinWeightsJointIndices(0, v, &zero, 1);
  }
  w->setBlendShapeChannelIndex(0, 0, 0);
  std::uint32_t vertex = 1;
  dna::Delta delta{0, 0, 1};
  w->setBlendShapeTargetVertexIndices(0, 0, &vertex, 1);
  w->setBlendShapeTargetDeltas(0, 0, &delta, 1);
  w->write();
  check_status("Synthetic DNA write failed");
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
Json self_test() {
  TemporaryDirectory tmp(fs::temp_directory_path());
  auto root = tmp.path;
  {
    std::ofstream f(root / "hash");
    f << "abc";
  }
  require(
      sha256(root / "hash") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA256 check failed");
  // Exercise non-topological parent order, translated parent axes and cycles.
  std::vector<float> neutral = {0, 2, 0, 0, 0, 0,          1,          1, 1, 1,
                                1, 0, 0, 0, 0, .70710677f, .70710677f, 1, 1, 1};
  std::vector<std::uint16_t> parents = {1, 1};
  auto world = joint_world({neutral.data(), neutral.size()}, parents);
  require(norm(transform(world[0], {0, 0, 0}) - Vec3{-1, 0, 0}) < 1e-6,
          "Joint hierarchy composition failed");
  bool rejected = false;
  try {
    joint_world({neutral.data(), neutral.size()}, {1, 0});
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  require(rejected, "Joint hierarchy cycle accepted");
  write_fixture(root / "head.dna");
  write_fixture(root / "body.dna", true);
  Json mapping = {{"control_space", "raw"}, {"targets", Json::object()}};
  for (const auto &name : target_names)
    mapping["targets"][name] = {{"controls", {{"CTRL_expressions.test", 1.0}}}};
  write_json(root / "mapping.json", mapping);
  Options o;
  o.input = root / "head.dna";
  o.body = root / "body.dna";
  o.mapping = root / "mapping.json";
  o.output = root / "bake.glb";
  auto report = bake(o, true);
  require(report["zero_targets"].empty() && report["target_names"].size() == 52,
          "Synthetic bake lost targets");
  std::ifstream f(o.output, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), {});
  auto get32 = [&](std::size_t offset) {
    require(offset + 4 <= bytes.size(), "Truncated GLB");
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
  };
  require(get32(0) == 0x46546c67 && get32(4) == 2 && get32(8) == bytes.size(),
          "Invalid synthetic GLB header");
  auto length = get32(12);
  require(20ull + length <= bytes.size(), "Invalid GLB JSON length");
  auto doc = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + length);
  require(doc["meshes"].size() == 2 &&
              !doc["meshes"][1]["primitives"][0].contains("targets"),
          "Synthetic body missing or has facial targets");
  const auto &primitive = doc["meshes"][0]["primitives"][0];
  require(primitive["targets"].size() == 52, "Synthetic GLB missing targets");
  for (const auto &target : primitive["targets"]) {
    auto accessor = doc["accessors"][target["POSITION"].get<std::size_t>()];
    auto view = doc["bufferViews"][accessor["bufferView"].get<std::size_t>()];
    auto offset = 28 + length + view["byteOffset"].get<std::size_t>();
    const double expected[] = {-.01, 0, 0, -.01, 0, -.01, -.01, 0, 0};
    for (std::size_t i = 0; i < 9; ++i) {
      auto bits = get32(offset + i * 4);
      float value;
      std::memcpy(&value, &bits, 4);
      require(std::abs(value - expected[i]) < 1e-7,
              "Synthetic joint or corrective deformation incorrect");
    }
  }
  o.body.clear();
  o.output = root / "baked.dna";
  auto dna_report = bake(o, true);
  require(dna_report["baked_dna"]["verification"]["verified"].get<bool>(),
          "Synthetic baked DNA failed verification");
  return {{"ok", true},
          {"test", "synthetic-DNA/RigLogic/skinning/GLB/baked-DNA"},
          {"targets", 52},
          {"body", true},
          {"baked_dna", true},
          {"openriglogic_version", rl4::VersionInfo::getVersionString()}};
}
} // namespace arkit
