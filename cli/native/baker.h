#pragma once
#include "targets.h"
#include "transforms.h"
#include "vendor/json.hpp"
#include <cctype>
#include <dna/BinaryStreamWriter.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <riglogic/RigLogic.h>
#include <string>
#include <utility>
#include <vector>
namespace arkit {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Pose = std::vector<std::vector<Vec3>>;
using Targets = std::vector<Pose>;
struct Corrective {
  std::uint16_t channel;
  std::vector<std::uint32_t> vertices;
  std::vector<Vec3> deltas;
};
struct Mesh {
  std::string name;
  std::vector<Vec3> positions;
  std::vector<std::uint32_t> layouts, triangles;
  std::vector<std::array<float, 2>> uvs;
  std::vector<std::vector<double>> weights;
  std::vector<std::vector<std::uint16_t>> joints;
  std::vector<Corrective> correctives;
};
std::vector<Mat4> joint_world(rl4::ConstArrayView<float> neutral,
                              const std::vector<std::uint16_t> &parents,
                              rl4::ConstArrayView<float> deltas = {});
struct Rig {
  // Destruction order matters: reader owns views used by logic; stream outlives
  // reader.
  rl4::ScopedPtr<rl4::FileStream> stream;
  rl4::ScopedPtr<rl4::BinaryStreamReader> reader;
  rl4::ScopedPtr<rl4::RigLogic> logic;
  std::vector<std::uint16_t> parents;
  std::vector<Mat4> bind_inverse;
  std::vector<std::string> gui_names, raw_names;
  double scale;
  explicit Rig(const fs::path &path);
  Pose evaluate(const std::vector<Mesh> &meshes, const Json &controls,
                const std::string &space) const;
};
Mesh read_mesh(const dna::Reader &reader, std::uint16_t index);
void check_status(const std::string &context);
void validate_behavior(const dna::Reader &reader);
void validate_mapping(const Json &mapping,
                      const std::vector<std::string> &controls);
void write_glb(const fs::path &path, const std::vector<Mesh> &meshes,
               const Pose &neutral, const Targets &targets,
               const std::vector<Mesh> &body = {},
               const Pose &body_neutral = {});
Json write_dna(const fs::path &path, const Rig &rig,
               const std::vector<std::uint16_t> &indices,
               const std::vector<Mesh> &meshes, const Pose &neutral,
               const Targets &targets);
struct Options {
  fs::path input, output, body, mapping;
  std::vector<std::string> meshes;
};
Json bake(const Options &options, bool quiet = false);
Json self_test();
void write_fixture(const fs::path &path, bool body = false);
std::string sha256(const fs::path &path);
Json read_json(const fs::path &path);
void write_json(const fs::path &path, const Json &value);
// A private temporary directory keeps failed writes away from existing outputs.
struct TemporaryDirectory {
  fs::path path;
  explicit TemporaryDirectory(const fs::path &parent);
  ~TemporaryDirectory();
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
};
void check_output_path(const fs::path &path);
void install_outputs(TemporaryDirectory &staging,
                     const std::vector<std::pair<fs::path, fs::path>> &files);
void replace_file(const fs::path &temporary, const fs::path &destination);
} // namespace arkit
