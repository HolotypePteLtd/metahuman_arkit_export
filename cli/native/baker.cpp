#include "baker.h"
#include "config.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <set>
namespace arkit {
void check_status(const std::string &context) {
  if (!rl4::Status::isOk())
    throw std::runtime_error(context + ": " + rl4::Status::get().message);
}
std::vector<Mat4> joint_world(rl4::ConstArrayView<float> neutral,
                              const std::vector<std::uint16_t> &parents,
                              rl4::ConstArrayView<float> deltas) {
  if (neutral.size() != parents.size() * 10 ||
      ((deltas.size() != 0) && deltas.size() != neutral.size()))
    throw std::runtime_error("Invalid joint transform count");
  std::vector<Mat4> local(parents.size()), world(parents.size());
  std::vector<int> state(parents.size());
  for (std::size_t i = 0; i < parents.size(); ++i) {
    auto n = neutral.data() + i * 10;
    auto r = quaternion(n + 3);
    if ((deltas.size() != 0))
      r = r * quaternion(deltas.data() + i * 10 + 3);
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col)
        r(row, col) *=
            double(n[7 + col]) +
            ((deltas.size() == 0) ? 0. : double(deltas[i * 10 + 7 + col]));
      r(row, 3) = double(n[row]) +
                  ((deltas.size() == 0) ? 0. : double(deltas[i * 10 + row]));
    }
    local[i] = r;
  }
  std::function<void(std::size_t)> visit = [&](std::size_t i) {
    if (state[i] == 1)
      throw std::runtime_error("Cycle in DNA joint hierarchy");
    if (state[i] == 2)
      return;
    state[i] = 1;
    auto p = parents[i];
    if (p == i || p == 65535)
      world[i] = local[i];
    else {
      if (p >= parents.size())
        throw std::runtime_error("Invalid joint parent index");
      visit(p);
      world[i] = world[p] * local[i];
    }
    state[i] = 2;
  };
  for (std::size_t i = 0; i < parents.size(); ++i)
    visit(i);
  return world;
}
Rig::Rig(const fs::path &path)
    : stream(rl4::makeScoped<rl4::FileStream>(
          path.u8string().c_str(), rl4::FileStream::AccessMode::Read,
          rl4::FileStream::OpenMode::Binary)),
      reader(rl4::makeScoped<rl4::BinaryStreamReader>(
          stream.get(),
          []() {
            dna::Configuration c;
            c.minLOD = c.maxLOD = 0;
            c.coordinateSystemTransformPolicy =
                dna::CoordinateSystemTransformPolicy::Transform;
            c.coordinateSystem = {dna::Direction::right, dna::Direction::up,
                                  dna::Direction::back};
            return c;
          }())),
      logic(nullptr) {
  reader->read();
  check_status("Cannot read DNA");
  validate_behavior(*reader);
  rl4::Configuration cfg;
  cfg.rotationType = rl4::RotationType::Quaternions;
  cfg.floatingPointType = rl4::FloatingPointType::Float;
  logic = rl4::makeScoped<rl4::RigLogic>(reader.get(), cfg);
  for (std::uint16_t i = 0; i < reader->getJointCount(); ++i)
    parents.push_back(reader->getJointParentIndex(i));
  auto bind = joint_world(logic->getNeutralJointValues(), parents);
  for (auto &m : bind)
    bind_inverse.push_back(inverse(m));
  scale = reader->getTranslationUnit() == dna::TranslationUnit::cm ? .01 : 1.;
  for (std::uint16_t i = 0; i < reader->getGUIControlCount(); ++i)
    gui_names.emplace_back(reader->getGUIControlName(i).c_str());
  for (std::uint16_t i = 0; i < reader->getRawControlCount(); ++i)
    raw_names.emplace_back(reader->getRawControlName(i).c_str());
}
Mesh read_mesh(const dna::Reader &r, std::uint16_t index) {
  Mesh m;
  m.name = r.getMeshName(index).c_str();
  auto count = r.getVertexPositionCount(index);
  if (!count || r.getSkinWeightsCount(index) != count)
    throw std::runtime_error(m.name + ": missing geometry or skin weights");
  auto xs = r.getVertexPositionXs(index), ys = r.getVertexPositionYs(index),
       zs = r.getVertexPositionZs(index);
  if (xs.size() != count || ys.size() != count || zs.size() != count)
    throw std::runtime_error(m.name + ": invalid position array lengths");
  auto maximum = r.getMaximumInfluencePerVertex(index);
  for (std::uint32_t v = 0; v < count; ++v) {
    m.positions.push_back({xs[v], ys[v], zs[v]});
    auto w = r.getSkinWeightsValues(index, v);
    auto j = r.getSkinWeightsJointIndices(index, v);
    if ((w.size() == 0) || w.size() != j.size() || w.size() > maximum)
      throw std::runtime_error(m.name + ": invalid skin weights");
    double sum = 0;
    for (std::size_t k = 0; k < w.size(); ++k) {
      if (!std::isfinite(w[k]) || w[k] < 0 || j[k] >= r.getJointCount())
        throw std::runtime_error(m.name + ": invalid skin influence");
      sum += w[k];
    }
    if (std::abs(sum - 1) > 1e-3)
      throw std::runtime_error(m.name + ": invalid weight sums");
    m.weights.emplace_back();
    for (auto value : w)
      m.weights.back().push_back(value / sum);
    m.joints.emplace_back(j.begin(), j.end());
  }
  auto layouts = r.getVertexLayoutPositionIndices(index),
       uvs = r.getVertexLayoutTextureCoordinateIndices(index);
  auto us = r.getVertexTextureCoordinateUs(index),
       vs = r.getVertexTextureCoordinateVs(index);
  if (layouts.size() != uvs.size() ||
      layouts.size() != r.getVertexLayoutNormalIndices(index).size() ||
      us.size() != vs.size() ||
      r.getVertexNormalXs(index).size() != r.getVertexNormalYs(index).size() ||
      r.getVertexNormalXs(index).size() != r.getVertexNormalZs(index).size())
    throw std::runtime_error(m.name +
                             ": invalid vertex attribute array lengths");
  for (std::size_t i = 0; i < layouts.size(); ++i) {
    if (layouts[i] >= count || uvs[i] >= us.size())
      throw std::runtime_error("Invalid vertex layout");
    m.layouts.push_back(layouts[i]);
    m.uvs.push_back({us[uvs[i]], 1.f - vs[uvs[i]]});
  }
  for (std::uint32_t f = 0; f < r.getFaceCount(index); ++f) {
    auto corners = r.getFaceVertexLayoutIndices(index, f);
    for (auto c : corners)
      if (c >= layouts.size())
        throw std::runtime_error("Invalid face vertex layout");
    for (std::size_t j = 1; j + 1 < corners.size(); ++j)
      m.triangles.insert(m.triangles.end(),
                         {corners[0], corners[j], corners[j + 1]});
  }
  if (m.layouts.empty() || m.triangles.empty())
    throw std::runtime_error(m.name + ": empty geometry");
  for (std::uint16_t t = 0; t < r.getBlendShapeTargetCount(index); ++t) {
    Corrective c;
    c.channel = r.getBlendShapeChannelIndex(index, t);
    auto vertices = r.getBlendShapeTargetVertexIndices(index, t);
    auto dx = r.getBlendShapeTargetDeltaXs(index, t),
         dy = r.getBlendShapeTargetDeltaYs(index, t),
         dz = r.getBlendShapeTargetDeltaZs(index, t);
    if (c.channel >= r.getBlendShapeChannelCount() ||
        dx.size() != vertices.size() || dy.size() != vertices.size() ||
        dz.size() != vertices.size())
      throw std::runtime_error(m.name + ": invalid corrective arrays");
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if (vertices[i] >= count)
        throw std::runtime_error("Invalid corrective vertex");
      c.vertices.push_back(vertices[i]);
      c.deltas.push_back({dx[i], dy[i], dz[i]});
    }
    m.correctives.push_back(std::move(c));
  }
  return m;
}
Pose Rig::evaluate(const std::vector<Mesh> &meshes, const Json &controls,
                   const std::string &space) const {
  auto instance = rl4::makeScoped<rl4::RigInstance>(logic.get());
  instance->setLOD(0);
  const auto &names = space == "gui" ? gui_names : raw_names;
  for (auto it = controls.begin(); it != controls.end(); ++it) {
    auto name = std::find(names.begin(), names.end(), it.key());
    if (name == names.end())
      throw std::runtime_error("Unknown control: " + it.key());
    auto i = static_cast<std::uint16_t>(name - names.begin());
    if (space == "gui")
      instance->setGUIControl(i, it.value().get<float>());
    else
      instance->setRawControl(i, it.value().get<float>());
  }
  if (space == "gui")
    logic->mapGUIToRawControls(instance.get());
  logic->calculate(instance.get());
  auto world = joint_world(logic->getNeutralJointValues(), parents,
                           instance->getJointOutputs());
  for (std::size_t i = 0; i < world.size(); ++i)
    world[i] = world[i] * bind_inverse[i];
  auto blends = instance->getBlendShapeOutputs();
  Pose pose;
  for (const auto &mesh : meshes) {
    auto positions = mesh.positions;
    for (const auto &c : mesh.correctives) {
      if (c.channel >= blends.size())
        throw std::runtime_error("Invalid corrective channel");
      auto w = blends[c.channel];
      if (std::abs(w) > 1e-10)
        for (std::size_t i = 0; i < c.vertices.size(); ++i)
          positions[c.vertices[i]] = positions[c.vertices[i]] + c.deltas[i] * w;
    }
    pose.emplace_back();
    for (std::size_t v = 0; v < positions.size(); ++v) {
      Vec3 p{};
      for (std::size_t k = 0; k < mesh.weights[v].size(); ++k)
        p = p + transform(world[mesh.joints[v][k]], positions[v]) *
                    mesh.weights[v][k];
      pose.back().push_back(p * scale);
    }
  }
  return pose;
}
void validate_mapping(const Json &m, const std::vector<std::string> &names) {
  auto space = m.value("control_space", std::string{});
  if (space != "gui" && space != "raw")
    throw std::runtime_error("mapping control_space must be 'gui' or 'raw'");
  if (!m.contains("targets") || !m["targets"].is_object() ||
      m["targets"].size() != 52)
    throw std::runtime_error(
        "mapping must contain exactly the 52 ARKit target names");
  for (const auto &name : target_names) {
    if (!m["targets"].contains(name))
      throw std::runtime_error("Missing ARKit target: " + name);
    const auto &recipe = m["targets"][name];
    if (!recipe.is_object() || !recipe.contains("controls") ||
        recipe["controls"].empty())
      throw std::runtime_error(name + ": nonempty controls required");
    for (auto key : {"controls", "subtract"})
      if (recipe.contains(key)) {
        if (!recipe[key].is_object())
          throw std::runtime_error(name + ": controls must be dictionaries");
        for (auto it = recipe[key].begin(); it != recipe[key].end(); ++it)
          if (std::find(names.begin(), names.end(), it.key()) == names.end() ||
              !it.value().is_number() ||
              !std::isfinite(it.value().get<double>()))
            throw std::runtime_error(
                name + ": unknown or invalid control: " + it.key());
      }
  }
}
Json bake(const Options &o, bool quiet) {
  auto extension = o.output.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  bool dna_output = extension == ".dna";
  if (!dna_output && extension != ".glb")
    throw std::runtime_error("output must have a .glb or .dna extension");
  if (dna_output && !o.body.empty())
    throw std::runtime_error("--body-dna is supported for GLB output only");
  auto report_path = o.output;
  report_path.replace_extension(".report.json");
  for (const auto &destination : {o.output, report_path}) {
    check_output_path(destination);
    for (const auto &source : {o.input, o.body, o.mapping}) {
      if (source.empty())
        continue;
      std::error_code ec;
      if (fs::weakly_canonical(source) == fs::weakly_canonical(destination) ||
          fs::equivalent(source, destination, ec))
        throw std::runtime_error(
            "output must differ from source DNA and mapping "
            "files; report must also differ: " +
            destination.u8string());
    }
  }
  Rig rig(o.input);
  auto &r = *rig.reader;
  Json mapping = o.mapping.empty() ? Json::parse(default_mapping_text)
                                   : read_json(o.mapping);
  auto space = mapping.at("control_space").get<std::string>();
  validate_mapping(mapping, space == "gui" ? rig.gui_names : rig.raw_names);
  std::vector<std::uint16_t> indices;
  std::vector<Mesh> meshes;
  std::set<std::string> missing(o.meshes.begin(), o.meshes.end());
  for (auto i : r.getMeshIndicesForLOD(0)) {
    std::string name = r.getMeshName(i).c_str();
    missing.erase(name);
    if (o.meshes.empty() ||
        std::find(o.meshes.begin(), o.meshes.end(), name) != o.meshes.end()) {
      indices.push_back(i);
      meshes.push_back(read_mesh(r, i));
    }
  }
  if (!missing.empty())
    throw std::runtime_error("Unknown LOD0 mesh name: " + *missing.begin());
  if (meshes.empty())
    throw std::runtime_error("DNA has no LOD0 meshes to bake");
  auto neutral = rig.evaluate(meshes, Json::object(), space);
  Targets targets;
  Json stats = Json::object(), zero = Json::array();
  int nonzero = 0;
  for (const auto &name : target_names) {
    const auto &recipe = mapping["targets"][name];
    auto pose = rig.evaluate(meshes, recipe["controls"], space);
    auto baseline = recipe.contains("subtract")
                        ? rig.evaluate(meshes, recipe["subtract"], space)
                        : neutral;
    double maximum = 0;
    Json maxima = Json::object();
    for (std::size_t m = 0; m < meshes.size(); ++m) {
      double local = 0;
      for (std::size_t v = 0; v < pose[m].size(); ++v) {
        pose[m][v] = pose[m][v] - baseline[m][v];
        local = std::max(local, norm(pose[m][v]));
      }
      maxima[meshes[m].name] = local;
      maximum = std::max(maximum, local);
    }
    stats[name] = {{"max_displacement_m", maximum}, {"meshes", maxima}};
    if (maximum <= 1e-6)
      zero.push_back(name);
    else
      ++nonzero;
    targets.push_back(std::move(pose));
    if (!quiet)
      std::cout << name << ": " << maximum * 1000 << " mm\n";
  }
  if (nonzero < 3)
    throw std::runtime_error(
        "Fewer than three nonzero targets; refusing to write an invalid bake");
  auto source_version = [](const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    unsigned char h[8]{};
    f.read(reinterpret_cast<char *>(h), 8);
    if (!f)
      throw std::runtime_error("Truncated DNA header");
    return Json::array({h[4] | (h[5] << 8), h[6] | (h[7] << 8)});
  };
  Json report = {{"source_dna", fs::canonical(o.input).u8string()},
                 {"source_sha256", sha256(o.input)},
                 {"openriglogic_version", rl4::VersionInfo::getVersionString()},
                 {"dna_version", source_version(o.input)},
                 {"reader_dna_version",
                  {r.getFileFormatGeneration(), r.getFileFormatVersion()}},
                 {"joint_count", r.getJointCount()},
                 {"gui_control_count", rig.gui_names.size()},
                 {"raw_control_count", rig.raw_names.size()},
                 {"blend_shape_channel_count", r.getBlendShapeChannelCount()},
                 {"coordinate_system", "right / up / back; metres; CCW"},
                 {"target_names", target_names},
                 {"mapping", mapping},
                 {"mapping_source", o.mapping.empty()
                                        ? "repository GUI control recipes"
                                        : fs::canonical(o.mapping).u8string()},
                 {"unreal_plugin_parity_verified", false},
                 {"zero_targets", zero},
                 {"meshes", Json::array()},
                 {"targets", stats}};
  auto describe = [](const Mesh &m) {
    return Json{{"name", m.name},
                {"positions", m.positions.size()},
                {"glb_vertices", m.layouts.size()},
                {"triangles", m.triangles.size() / 3},
                {"dna_corrective_targets", m.correctives.size()}};
  };
  for (const auto &m : meshes)
    report["meshes"].push_back(describe(m));
  std::vector<Mesh> body;
  Pose body_neutral;
  if (!o.body.empty()) {
    Rig b(o.body);
    for (auto i : b.reader->getMeshIndicesForLOD(0))
      body.push_back(read_mesh(*b.reader, i));
    if (body.empty())
      throw std::runtime_error("Body DNA has no LOD0 meshes");
    body_neutral = b.evaluate(body, Json::object(), "raw");
    report["body"] = {{"source_dna", fs::canonical(o.body).u8string()},
                      {"source_sha256", sha256(o.body)},
                      {"dna_version", source_version(o.body)},
                      {"joint_count", b.reader->getJointCount()},
                      {"pose", "evaluated neutral"},
                      {"skeletal_skin_exported", false}};
    for (const auto &m : body) {
      auto d = describe(m);
      d["component"] = "body";
      d["morph_target_count"] = 0;
      report["meshes"].push_back(d);
    }
  }
  auto parent = o.output.parent_path();
  if (parent.empty())
    parent = ".";
  fs::create_directories(parent);
  TemporaryDirectory staging(parent);
  auto staged_output =
      staging.path / (dna_output ? "output.dna" : "output.glb");
  auto staged_report = staging.path / "report.json";
  if (dna_output) {
    auto verification =
        write_dna(staged_output, rig, indices, meshes, neutral, targets);
    Json raw = Json::array();
    for (auto name : target_names) {
      name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
      raw.push_back("CTRL_expressions." + name);
    }
    report["baked_dna"] = {
        {"lod_count", 1},
        {"skeleton_pose", "neutral; expression motion baked into blendshapes"},
        {"gui_control_names", target_names},
        {"raw_control_names", raw},
        {"blend_shape_channel_count", 52},
        {"verification", verification}};
  } else {
    write_glb(staged_output, meshes, neutral, targets, body, body_neutral);
  }
  write_json(staged_report, report);
  install_outputs(staging,
                  {{staged_output, o.output}, {staged_report, report_path}});
  if (!quiet)
    std::cout << "Wrote " << o.output.u8string()
              << "\nReport: " << report_path.u8string() << "\n";
  return report;
}
} // namespace arkit
