#include "baker.h"
#include "config.h"
#include "process.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
namespace arkit::test {
void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F function, const std::string &message) {
  try {
    function();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance,
          const std::string &message) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          message + ": actual=" + std::to_string(actual) +
              ", expected=" + std::to_string(expected));
}
std::vector<Mesh> meshes(Rig &rig) {
  std::vector<Mesh> values;
  for (auto i : rig.reader->getMeshIndicesForLOD(0))
    values.push_back(read_mesh(*rig.reader, i));
  return values;
}
Pose pose(const Json &value) {
  Pose p;
  for (const auto &mesh : value) {
    p.emplace_back();
    for (const auto &v : mesh)
      p.back().push_back(v.get<Vec3>());
  }
  return p;
}
Targets targets(const Json &value) {
  Targets t;
  for (const auto &p : value)
    t.push_back(pose(p));
  return t;
}
void compare(const Pose &actual, const Pose &expected, double tolerance) {
  require(actual.size() == expected.size(), "Pose mesh count changed");
  for (std::size_t m = 0; m < actual.size(); ++m) {
    require(actual[m].size() == expected[m].size(),
            "Pose vertex count changed");
    for (std::size_t v = 0; v < actual[m].size(); ++v)
      near(norm(actual[m][v] - expected[m][v]), 0, tolerance,
           "Evaluated geometry differs from reference");
  }
}
struct Context {
  fs::path fixtures = fs::u8path(MHARKIT_TEST_FIXTURES), binary;
  TemporaryDirectory temporary{fs::temp_directory_path()};
  Json expected = read_json(fixtures / "expected.json");
  Options options;
  explicit Context(fs::path executable) : binary(std::move(executable)) {
    options.input = fixtures / "head.dna";
    options.mapping = fixtures / "mapping-raw.json";
    options.output = temporary.path / "baked.dna";
  }
};
struct GLB {
  std::vector<unsigned char> bytes;
  Json document;
  std::size_t start;
  explicit GLB(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), {});
    require(u32(0) == 0x46546c67 && u32(4) == 2 && u32(8) == bytes.size(),
            "Invalid GLB header");
    auto count = u32(12);
    require(u32(16) == 0x4e4f534a && 20ull + count + 8 <= bytes.size(),
            "Invalid JSON chunk");
    document = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + count);
    start = 28 + count;
    require(u32(24 + count) == 0x004e4942, "Invalid binary chunk");
    require(start + u32(20 + count) == bytes.size(), "Invalid binary length");
  }
  std::uint32_t u32(std::size_t offset) const {
    require(offset + 4 <= bytes.size(), "GLB accessor exceeds buffer");
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
  }
  std::vector<double> accessor(std::size_t index, int width,
                               bool indices = false) const {
    const auto &a = document.at("accessors").at(index);
    require(a["type"] == (width == 3   ? "VEC3"
                          : width == 2 ? "VEC2"
                                       : "SCALAR"),
            "Wrong accessor type");
    require(a["componentType"] == (indices ? 5125 : 5126),
            "Wrong component type");
    const auto &view =
        document.at("bufferViews").at(a["bufferView"].get<std::size_t>());
    auto offset = start + view.value("byteOffset", std::size_t(0)) +
                  a.value("byteOffset", std::size_t(0));
    std::vector<double> values;
    for (std::size_t i = 0; i < a["count"].get<std::size_t>() * width; ++i) {
      auto bits = u32(offset + 4 * i);
      float value;
      std::memcpy(&value, &bits, 4);
      values.push_back(indices ? double(bits) : double(value));
    }
    return values;
  }
};
void joint_composition(Context &) {
  std::vector<float> neutral = {10,         20,         30, 0, 0,
                                .70710677f, .70710677f, 1,  1, 1};
  std::vector<float> delta = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0};
  auto world = joint_world({neutral.data(), neutral.size()}, {0},
                           {delta.data(), delta.size()});
  near(norm(transform(world[0], {0, 0, 0}) - Vec3{11, 20, 30}), 0, 1e-12,
       "Joint translation rotated incorrectly");
  std::vector<float> n;
  for (auto position : {Vec3{1, 0, 0}, Vec3{0, 2, 0}, Vec3{0, 0, 3}}) {
    for (auto v : position)
      n.push_back(float(v));
    n.insert(n.end(), {0, 0, 0, 1, 1, 1, 1});
  }
  auto w = joint_world({n.data(), n.size()}, {2, 1, 1});
  near(norm(transform(w[0], {0, 0, 0}) - Vec3{1, 2, 3}), 0, 1e-12,
       "Non-topological hierarchy failed");
  rejects([&] { joint_world({n.data(), n.size()}, {1, 2, 0}); },
          "Cycle accepted");
  rejects([&] { joint_world({n.data(), n.size()}, {4, 1, 1}); },
          "Invalid parent accepted");
  neutral[6] = neutral[5] = 0;
  rejects([&] { joint_world({neutral.data(), neutral.size()}, {0}); },
          "Zero quaternion accepted");
}
void recipes(Context &c) {
  auto mapping = Json::parse(default_mapping_text);
  require(mapping == c.expected["default_mapping"],
          "Default expression recipes differ from the reference");
  require(Json(target_names) == c.expected["target_names"],
          "ARKit target order changed");
  std::vector<std::string> names;
  for (const auto &recipe : mapping["targets"])
    for (auto key : {"controls", "subtract"})
      if (recipe.contains(key))
        for (auto it = recipe[key].begin(); it != recipe[key].end(); ++it)
          names.push_back(it.key());
  validate_mapping(mapping, names);
  names.erase(std::remove(names.begin(), names.end(), "CTRL_L_eye_blink.ty"),
              names.end());
  rejects([&] { validate_mapping(mapping, names); },
          "Unknown mapping control accepted");
  Rig rig(c.options.input);
  auto custom = read_json(c.options.mapping);
  custom["targets"]["JawOpen"]["controls"] = Json::array({1});
  rejects([&] { validate_mapping(custom, rig.raw_names); },
          "Malformed controls accepted");
  custom = read_json(c.options.mapping);
  custom["targets"].erase("JawOpen");
  rejects([&] { validate_mapping(custom, rig.raw_names); },
          "Incomplete targets accepted");
}
void evaluated_targets(Context &c) {
  require(sha256(c.options.input) == c.expected["provenance"]["source_sha256"],
          "Golden fixture bytes changed");
  Rig rig(c.options.input);
  auto m = meshes(rig);
  auto neutral = pose(c.expected["neutral"]);
  auto expected = targets(c.expected["targets"]);
  for (auto space : {"raw", "gui"}) {
    auto mapping =
        read_json(c.fixtures / (std::string("mapping-") + space + ".json"));
    compare(rig.evaluate(m, Json::object(), space), neutral, 1e-7);
    for (std::size_t t = 0; t < 52; ++t) {
      const auto &r = mapping["targets"][target_names[t]];
      auto actual = rig.evaluate(m, r["controls"], space);
      auto baseline = r.contains("subtract")
                          ? rig.evaluate(m, r["subtract"], space)
                          : neutral;
      for (std::size_t i = 0; i < actual.size(); ++i)
        for (std::size_t v = 0; v < actual[i].size(); ++v)
          actual[i][v] = actual[i][v] - baseline[i][v];
      compare(actual, expected[t], 1e-7);
    }
  }
}
void serialized_glb(Context &c) {
  for (auto space : {"raw", "gui"}) {
    c.options.mapping =
        c.fixtures / (std::string("mapping-") + space + ".json");
    c.options.output = c.temporary.path / "output.glb";
    c.options.body = c.fixtures / "body.dna";
    auto report = bake(c.options, true);
    GLB glb(c.options.output);
    require(glb.document["meshes"].size() == 3, "Missing head or body meshes");
    require(glb.document["scenes"][0]["nodes"] == Json::array({0, 1, 2}),
            "Scene nodes changed");
    for (std::size_t i = 0; i < 2; ++i) {
      const auto &m = glb.document["meshes"][i];
      const auto &frozen = c.expected["meshes"][i];
      require(m["name"] == frozen["name"] &&
                  m["extras"]["targetNames"] == c.expected["target_names"],
              "GLB names changed");
      auto p = m["primitives"][0];
      require(p["targets"].size() == 52, "GLB lost zero targets");
      auto indices = glb.accessor(p["indices"], 1, true);
      require(Json(indices) == frozen["triangles"],
              "Triangulated polygon indices changed");
      auto uv = glb.accessor(p["attributes"]["TEXCOORD_0"], 2);
      for (std::size_t v = 0; v < 4; ++v)
        for (int a = 0; a < 2; ++a)
          near(uv[v * 2 + a], frozen["uvs"][v][a], 0, "UV seam changed");
      for (int t = -1; t < 52; ++t) {
        auto positions =
            glb.accessor(t < 0 ? p["attributes"]["POSITION"].get<std::size_t>()
                               : p["targets"][t]["POSITION"].get<std::size_t>(),
                         3);
        require(positions.size() == 12, "UV seam vertices not expanded");
        for (std::size_t v = 0; v < 4; ++v) {
          auto source = frozen["layouts"][v].get<std::size_t>();
          for (int a = 0; a < 3; ++a) {
            double value =
                t < 0 ? c.expected["neutral"][i][source][a].get<double>()
                      : c.expected["targets"][t][i][source][a].get<double>();
            near(positions[v * 3 + a], value, 1e-7,
                 "Serialized vertex differs from reference");
          }
        }
      }
    }
    auto body = glb.document["meshes"][2];
    require(!body.contains("weights") &&
                !body["primitives"][0].contains("targets"),
            "Body has facial morph targets");
    auto positions =
        glb.accessor(body["primitives"][0]["attributes"]["POSITION"], 3);
    for (int v = 0; v < 3; ++v)
      for (int a = 0; a < 3; ++a)
        near(positions[v * 3 + a], c.expected["body_neutral"][0][v][a], 1e-7,
             "Body neutral geometry changed");
    require(report["zero_targets"] == Json::array({"TongueOut", "MouthClose"}),
            "Zero target reporting changed");
    require(report["source_sha256"] ==
                c.expected["provenance"]["source_sha256"],
            "Source digest changed");
  }
}
template <class A, class B> void equal_view(A a, B b, const char *message) {
  require(a.size() == b.size(), message);
  for (std::size_t i = 0; i < a.size(); ++i)
    require(a[i] == b[i], message);
}
void serialized_dna(Context &c) {
  auto report = bake(c.options, true);
  require(report["baked_dna"]["verification"]["poses"] == 56,
          "Missing verification poses");
  Rig source(c.options.input), actual(c.options.output);
  auto &r = *actual.reader;
  require(r.getLODCount() == 1 && r.getMeshCount() == 2 &&
              r.getPSDCount() == 0 && r.getJointGroupCount() == 0,
          "Baked DNA behavior not replaced");
  require(Json(actual.gui_names) == c.expected["target_names"] &&
              actual.raw_names.size() == 52,
          "Controls changed");
  require(actual.parents == source.parents, "Joint hierarchy changed");
  auto joints = actual.logic->getNeutralJointValues();
  for (std::size_t i = 0; i < joints.size(); ++i)
    near(joints[i], c.expected["neutral_joints"][i / 10][i % 10], 1e-6,
         "Neutral joints changed");
  for (std::uint16_t m = 0; m < 2; ++m) {
    require(r.getBlendShapeTargetCount(m) == 52, "DNA lost named targets");
    require(r.getBlendShapeTargetDeltaCount(m, 51) == 0,
            "Zero target became nonzero");
    equal_view(r.getVertexTextureCoordinateUs(m),
               source.reader->getVertexTextureCoordinateUs(m),
               "DNA UVs changed");
    equal_view(r.getVertexTextureCoordinateVs(m),
               source.reader->getVertexTextureCoordinateVs(m),
               "DNA UVs changed");
    equal_view(r.getVertexNormalXs(m), source.reader->getVertexNormalXs(m),
               "DNA normals changed");
    equal_view(r.getVertexNormalYs(m), source.reader->getVertexNormalYs(m),
               "DNA normals changed");
    equal_view(r.getVertexNormalZs(m), source.reader->getVertexNormalZs(m),
               "DNA normals changed");
    equal_view(r.getVertexLayoutPositionIndices(m),
               source.reader->getVertexLayoutPositionIndices(m),
               "DNA layouts changed");
    equal_view(r.getVertexLayoutTextureCoordinateIndices(m),
               source.reader->getVertexLayoutTextureCoordinateIndices(m),
               "DNA UV layouts changed");
    equal_view(r.getVertexLayoutNormalIndices(m),
               source.reader->getVertexLayoutNormalIndices(m),
               "DNA normal layouts changed");
    equal_view(r.getFaceVertexLayoutIndices(m, 0),
               source.reader->getFaceVertexLayoutIndices(m, 0),
               "DNA polygons changed");
    for (std::uint32_t v = 0; v < 3; ++v) {
      equal_view(r.getSkinWeightsValues(m, v),
                 source.reader->getSkinWeightsValues(m, v),
                 "Skin weights changed");
      equal_view(r.getSkinWeightsJointIndices(m, v),
                 source.reader->getSkinWeightsJointIndices(m, v),
                 "Skin joints changed");
    }
  }
  auto m = meshes(actual);
  auto neutral = pose(c.expected["neutral"]);
  auto deltas = targets(c.expected["targets"]);
  std::mt19937 random(0);
  for (int p = 0; p < 56; ++p) {
    std::array<double, 52> weights{};
    if (p > 0 && p <= 52)
      weights[p - 1] = 1;
    else if (p > 52)
      for (auto &w : weights)
        w = double(random()) / std::mt19937::max();
    auto expected = neutral;
    Json raw = Json::object(), gui = Json::object();
    for (int t = 0; t < 52; ++t) {
      raw[actual.raw_names[t]] = weights[t];
      gui[actual.gui_names[t]] = weights[t];
      for (std::size_t mesh = 0; mesh < expected.size(); ++mesh)
        for (std::size_t v = 0; v < expected[mesh].size(); ++v)
          expected[mesh][v] =
              expected[mesh][v] + deltas[t][mesh][v] * weights[t];
    }
    compare(actual.evaluate(m, raw, "raw"), expected, 2e-6);
    compare(actual.evaluate(m, gui, "gui"), expected, 2e-6);
  }
}
void mesh_selection(Context &c) {
  c.options.meshes = {"second_mesh"};
  bake(c.options, true);
  Rig rig(c.options.output);
  require(rig.reader->getMeshCount() == 1 &&
              std::string(rig.reader->getMeshName(0).c_str()) == "second_mesh",
          "Selected mesh not remapped");
  for (std::uint16_t i = 0; i < 52; ++i) {
    auto mapping = rig.reader->getMeshBlendShapeChannelMapping(i);
    require(mapping.meshIndex == 0 && mapping.blendShapeChannelIndex == i,
            "Channel mapping not remapped");
  }
}
void atomic_failure(Context &c) {
  {
    std::ofstream f(c.options.output);
    f << "preserve existing output";
  }
  auto before = sha256(c.options.output);
  Rig rig(c.options.input);
  auto m = meshes(rig);
  auto neutral = pose(c.expected["neutral"]);
  auto t = targets(c.expected["targets"]);
  std::vector<std::uint16_t> indices{0, 1};
  t[0][0][0][0] = 1e8 + .0123456789;
  rejects([&] { write_dna(c.options.output, rig, indices, m, neutral, t); },
          "Imprecise DNA passed verification");
  require(sha256(c.options.output) == before,
          "Failed verification overwrote existing output");
  auto mapping = read_json(c.options.mapping);
  mapping["targets"]["JawOpen"]["controls"] = {{"missing_control", 1}};
  c.options.mapping = c.temporary.path / "bad.json";
  write_json(c.options.mapping, mapping);
  rejects([&] { bake(c.options, true); }, "Unknown control accepted");
  require(sha256(c.options.output) == before,
          "Invalid mapping overwrote existing output");
  require(!fs::exists(c.temporary.path / "baked.report.json"),
          "Failed bake wrote success report");
}
void native_roundtrip(Context &c) {
  auto path = c.temporary.path / "roundtrip.dna";
  {
    auto stream = rl4::makeScoped<rl4::FileStream>(
        c.options.input.u8string().c_str(), rl4::FileStream::AccessMode::Read,
        rl4::FileStream::OpenMode::Binary);
    auto reader = rl4::makeScoped<rl4::BinaryStreamReader>(stream.get());
    reader->read();
    check_status("Read fixture");
    require(reader->getLODCount() == 2, "Roundtrip fixture lost LODs");
    auto output = rl4::makeScoped<rl4::FileStream>(
        path.u8string().c_str(), rl4::FileStream::AccessMode::Write,
        rl4::FileStream::OpenMode::Binary);
    auto writer = rl4::makeScoped<dna::BinaryStreamWriter>(output.get());
    writer->setFrom(reader.get());
    writer->write();
    check_status("Roundtrip fixture");
  }
  require(sha256(path) == sha256(c.options.input),
          "Native roundtrip changed bytes");
}
void cli_validation(Context &c) {
  auto source = c.options.input.u8string(),
       mapping = c.options.mapping.u8string();
  auto version = execute({c.binary.u8string(), "--version"}, c.temporary.path);
  require(version.status == 0 && version.output.find("bake-arkit-dna ") == 0,
          "Wrong CLI name or version");
  auto help = execute({c.binary.u8string(), "--help"}, c.temporary.path);
  require(help.status == 0 && help.output.find("--mesh") != std::string::npos,
          "Missing CLI help");
  auto copy = c.temporary.path / "source.dna";
  fs::copy_file(c.options.input, copy);
  auto digest = sha256(copy);
  auto overwrite = execute({c.binary.u8string(), copy.u8string(), "--mapping",
                            mapping, "-o", copy.u8string()}, c.temporary.path);
  require(overwrite.status != 0 &&
              overwrite.output.find("output must differ") != std::string::npos,
          "CLI overwrote source");
  require(sha256(copy) == digest, "Source modified");
  require(execute({c.binary.u8string(), source, "--mapping", mapping,
                   "--body-dna", (c.fixtures / "body.dna").u8string(), "-o",
                   c.options.output.u8string()},
                  c.temporary.path)
                  .status != 0,
          "Body accepted for DNA output");
  require(execute({c.binary.u8string(), source, "--mapping", mapping, "--mesh",
                   "unknown_mesh", "-o", c.options.output.u8string()},
                  c.temporary.path)
                  .status != 0,
          "Unknown mesh accepted");
}
void standalone_unicode(Context &c) {
  auto directory = c.temporary.path / fs::u8path("character café 角色");
  fs::create_directory(directory);
  auto program = directory / c.binary.filename();
  fs::copy_file(c.binary, program);
  auto result = execute({program.u8string(), "--self-test"}, directory, true);
  require(result.status == 0,
          "Copied executable failed without PATH: " + result.output);
  require(Json::parse(result.output)["ok"].get<bool>(),
          "Standalone self-test failed");
  auto input = directory / fs::u8path("tête.dna"),
       output = directory / fs::u8path("烘焙.glb");
  fs::copy_file(c.options.input, input);
  result = execute({program.u8string(), input.u8string(), "--mapping",
                    c.options.mapping.u8string(), "-o", output.u8string()},
                   directory, true);
  require(result.status == 0 && fs::exists(output),
          "Unicode bake failed: " + result.output);
  auto report = output;
  report.replace_extension(".report.json");
  require(read_json(report)["source_dna"] == fs::canonical(input).u8string(),
          "Unicode report path changed");
  require(!fs::exists(directory / "_internal"), "Runtime directory created");
}
} // namespace arkit::test
int main(int argc, char **argv) {
  using namespace arkit::test;
  try {
    if (argc != 3)
      throw std::runtime_error("Expected executable path and test name");
    Context context(arkit::fs::absolute(arkit::fs::u8path(argv[1])));
    const std::map<std::string, std::function<void(Context &)>> cases = {
        {"joints", joint_composition},
        {"recipes", recipes},
        {"evaluated-targets", evaluated_targets},
        {"serialized-glb", serialized_glb},
        {"serialized-dna", serialized_dna},
        {"mesh-selection", mesh_selection},
        {"atomic-failure", atomic_failure},
        {"roundtrip", native_roundtrip},
        {"cli-validation", cli_validation},
        {"standalone-unicode", standalone_unicode}};
    cases.at(argv[2])(context);
    std::cout << "PASS: " << argv[2] << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
