#include "baker.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif
namespace arkit {
Json read_json(const fs::path &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("Cannot open " + path.u8string());
  Json j;
  f >> j;
  return j;
}
void write_json(const fs::path &path, const Json &j) {
  // Match the reference report encoding, including Windows readers whose
  // default text encoding may not be UTF-8.
  auto encoded = j.dump(2, ' ', true);
  std::ofstream f(path);
  f << encoded << '\n';
  f.close();
  if (!f)
    throw std::runtime_error("Cannot write " + path.u8string());
}
TemporaryDirectory::TemporaryDirectory(const fs::path &parent) {
  std::random_device rng;
  for (int i = 0; i < 100; ++i) {
    auto candidate = parent / (".arkit-" + std::to_string(rng()) + "-" +
                               std::to_string(rng()));
#ifdef _WIN32
    bool created = fs::create_directory(candidate);
#else
    // Create with owner-only access atomically, regardless of the user's umask.
    bool created = ::mkdir(candidate.c_str(), 0700) == 0;
    if (!created && errno != EEXIST)
      throw fs::filesystem_error(
          "Cannot create private temporary directory", candidate,
          std::error_code(errno, std::generic_category()));
#endif
    if (created) {
      path = candidate;
      return;
    }
  }
  throw std::runtime_error("Cannot create private temporary directory");
}
TemporaryDirectory::~TemporaryDirectory() {
  std::error_code ec;
  if (!path.empty())
    fs::remove_all(path, ec);
}
void check_output_path(const fs::path &path) {
  std::error_code ec;
  auto status = fs::symlink_status(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    throw fs::filesystem_error("Cannot inspect output path", path, ec);
  if (status.type() != fs::file_type::not_found && !fs::is_regular_file(status))
    throw std::runtime_error(
        "Output path must be a regular file, not a symlink "
        "or directory: " +
        path.u8string());
}
void replace_file(const fs::path &source, const fs::path &destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot install output file");
#else
  fs::rename(source, destination);
#endif
}
void install_outputs(TemporaryDirectory &staging,
                     const std::vector<std::pair<fs::path, fs::path>> &files) {
  struct Previous {
    fs::path backup;
    bool saved = false, installed = false;
  };
  std::vector<Previous> previous(files.size());
  // Reject invalid destinations before touching any existing output.
  for (const auto &file : files)
    check_output_path(file.second);
  try {
    for (std::size_t i = 0; i < files.size(); ++i) {
      auto &p = previous[i];
      p.backup = staging.path / ("previous-" + std::to_string(i));
      // Rename backups instead of copying them: this preserves metadata and
      // works on filesystems without hard-link support.
      if (fs::exists(files[i].second)) {
        fs::rename(files[i].second, p.backup);
        p.saved = true;
      }
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
      replace_file(files[i].first, files[i].second);
      previous[i].installed = true;
    }
  } catch (...) {
    auto failure = std::current_exception();
    // Retain backups by default during recovery, including if allocating an
    // error message itself fails. Re-enable cleanup only after restoration.
    auto retained = std::move(staging.path);
    staging.path.clear();
    std::string restore_error;
    for (std::size_t i = files.size(); i-- > 0;) {
      try {
        if (previous[i].saved)
          replace_file(previous[i].backup, files[i].second);
        else if (previous[i].installed)
          fs::remove(files[i].second);
      } catch (const std::exception &e) {
        restore_error += std::string(e.what()) + "\n";
      }
    }
    if (!restore_error.empty()) {
      // Never let cleanup delete the user's backups if restoration failed.
      throw std::runtime_error("Cannot restore previous outputs; backups kept "
                               "at " +
                               retained.u8string() + ":\n" + restore_error);
    }
    staging.path = std::move(retained);
    std::rethrow_exception(failure);
  }
}
static void u32(std::vector<unsigned char> &b, std::uint32_t x) {
  for (int i = 0; i < 4; ++i)
    b.push_back(static_cast<unsigned char>(x >> (8 * i)));
}
void write_glb(const fs::path &path, const std::vector<Mesh> &meshes,
               const Pose &neutral, const Targets &targets,
               const std::vector<Mesh> &body, const Pose &body_neutral) {
  std::vector<unsigned char> blob;
  Json doc = {{"asset",
               {{"version", "2.0"},
                {"generator", "bake-arkit-dna C++ / OpenRigLogic"}}},
              {"scene", 0},
              {"scenes", Json::array({{{"nodes", Json::array()}}})},
              {"nodes", Json::array()},
              {"meshes", Json::array()},
              {"accessors", Json::array()},
              {"bufferViews", Json::array()}};
  auto add = [&](const std::vector<double> &values, int width,
                 bool indices = false) {
    if (values.empty() || values.size() % width)
      throw std::runtime_error("Empty or invalid GLB accessor");
    auto start = blob.size();
    std::vector<double> minimum(width, std::numeric_limits<double>::infinity()),
        maximum(width, -std::numeric_limits<double>::infinity());
    for (std::size_t i = 0; i < values.size(); ++i) {
      std::uint32_t bits;
      double value;
      if (indices) {
        bits = static_cast<std::uint32_t>(values[i]);
        value = bits;
      } else {
        float f = static_cast<float>(values[i]);
        value = f;
        std::memcpy(&bits, &f, 4);
      }
      if (!std::isfinite(value))
        throw std::runtime_error("Non-finite values in baked geometry");
      u32(blob, bits);
      auto c = i % width;
      minimum[c] = std::min(minimum[c], value);
      maximum[c] = std::max(maximum[c], value);
    }
    auto view = doc["bufferViews"].size();
    doc["bufferViews"].push_back({{"buffer", 0},
                                  {"byteOffset", start},
                                  {"byteLength", blob.size() - start},
                                  {"target", indices ? 34963 : 34962}});
    auto index = doc["accessors"].size();
    doc["accessors"].push_back({{"bufferView", view},
                                {"componentType", indices ? 5125 : 5126},
                                {"count", values.size() / width},
                                {"type", width == 3   ? "VEC3"
                                         : width == 2 ? "VEC2"
                                                      : "SCALAR"},
                                {"min", minimum},
                                {"max", maximum}});
    return index;
  };
  auto positions = [&](const Mesh &m, const std::vector<Vec3> &p) {
    std::vector<double> values;
    values.reserve(m.layouts.size() * 3);
    for (auto i : m.layouts)
      values.insert(values.end(), p.at(i).begin(), p.at(i).end());
    return add(values, 3);
  };
  for (std::size_t i = 0; i < meshes.size() + body.size(); ++i) {
    bool face = i < meshes.size();
    const auto &m = face ? meshes[i] : body[i - meshes.size()];
    const auto &p = face ? neutral[i] : body_neutral[i - meshes.size()];
    std::vector<double> uv;
    for (const auto &row : m.uvs)
      for (auto v : row)
        uv.push_back(v);
    std::vector<double> triangles(m.triangles.begin(), m.triangles.end());
    Json primitive = {
        {"attributes",
         {{"POSITION", positions(m, p)}, {"TEXCOORD_0", add(uv, 2)}}},
        {"indices", add(triangles, 1, true)}};
    Json entry = {{"name", m.name}};
    if (face) {
      primitive["targets"] = Json::array();
      for (const auto &t : targets)
        primitive["targets"].push_back({{"POSITION", positions(m, t[i])}});
      entry["weights"] = std::vector<double>(52);
      entry["extras"] = {{"targetNames", target_names}};
    }
    entry["primitives"] = Json::array({primitive});
    doc["meshes"].push_back(entry);
    doc["nodes"].push_back({{"mesh", i}, {"name", m.name}});
    doc["scenes"][0]["nodes"].push_back(i);
  }
  doc["buffers"] = Json::array({{{"byteLength", blob.size()}}});
  auto encoded = doc.dump();
  while (encoded.size() % 4)
    encoded += ' ';
  auto total = 28ull + encoded.size() + blob.size();
  if (total > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("GLB exceeds the 4 GiB format limit");
  std::vector<unsigned char> header;
  u32(header, 0x46546c67);
  u32(header, 2);
  u32(header, static_cast<std::uint32_t>(total));
  u32(header, static_cast<std::uint32_t>(encoded.size()));
  u32(header, 0x4e4f534a);
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(header.data()), header.size());
  f.write(encoded.data(), encoded.size());
  header.clear();
  u32(header, static_cast<std::uint32_t>(blob.size()));
  u32(header, 0x004e4942);
  f.write(reinterpret_cast<const char *>(header.data()), header.size());
  f.write(reinterpret_cast<const char *>(blob.data()), blob.size());
  f.close();
  if (!f)
    throw std::runtime_error("Cannot write GLB");
}
Json write_dna(const fs::path &path, const Rig &rig,
               const std::vector<std::uint16_t> &indices,
               const std::vector<Mesh> &meshes, const Pose &neutral,
               const Targets &targets) {
  auto parent = path.parent_path();
  if (parent.empty())
    parent = ".";
  TemporaryDirectory tmp(parent);
  auto temporary = tmp.path / "baked.dna";
  const auto &r = *rig.reader;
  {
    auto stream = rl4::makeScoped<rl4::FileStream>(
        temporary.u8string().c_str(), rl4::FileStream::AccessMode::Write,
        rl4::FileStream::OpenMode::Binary);
    auto w = rl4::makeScoped<dna::BinaryStreamWriter>(stream.get());
    w->setFrom(rig.reader.get(), dna::DataLayer::Geometry,
               dna::UnknownLayerPolicy::Ignore);
    w->clearGUIControlNames();
    w->clearRawControlNames();
    w->clearBlendShapeChannelNames();
    w->clearBlendShapeChannelIndices();
    w->clearLODBlendShapeChannelMappings();
    w->clearAnimatedMapNames();
    w->clearAnimatedMapIndices();
    w->clearLODAnimatedMapMappings();
    w->clearMeshNames();
    w->clearMeshIndices();
    w->clearLODMeshMappings();
    w->clearMeshBlendShapeChannelMappings();
    w->clearMeshes();
    w->setLODCount(1);
    w->setJointRowCount(r.getJointCount() * 9);
    w->setJointColumnCount(52);
    w->setPSDCount(0);
    for (std::uint16_t i = 0; i < 52; ++i) {
      auto name = target_names[i];
      w->setGUIControlName(i, name.c_str());
      w->setBlendShapeChannelName(i, name.c_str());
      name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
      w->setRawControlName(i, ("CTRL_expressions." + name).c_str());
    }
    std::vector<std::uint16_t> identity(52);
    std::iota(identity.begin(), identity.end(), 0);
    std::vector<float> zeros(52), ones(52, 1.f);
    w->setGUIToRawInputIndices(identity.data(), 52);
    w->setGUIToRawOutputIndices(identity.data(), 52);
    w->setGUIToRawFromValues(zeros.data(), 52);
    w->setGUIToRawToValues(ones.data(), 52);
    w->setGUIToRawSlopeValues(ones.data(), 52);
    w->setGUIToRawCutValues(zeros.data(), 52);
    w->setBlendShapeChannelIndices(0, identity.data(), 52);
    w->setLODBlendShapeChannelMapping(0, 0);
    std::uint16_t count = 52, zero = 0;
    w->setBlendShapeChannelLODs(&count, 1);
    w->setBlendShapeChannelInputIndices(identity.data(), 52);
    w->setBlendShapeChannelOutputIndices(identity.data(), 52);
    w->setAnimatedMapIndices(0, nullptr, 0);
    w->setLODAnimatedMapMapping(0, 0);
    w->setAnimatedMapLODs(&zero, 1);
    std::vector<std::uint16_t> mi(meshes.size());
    std::iota(mi.begin(), mi.end(), 0);
    w->setMeshIndices(0, mi.data(), static_cast<std::uint16_t>(mi.size()));
    w->setLODMeshMapping(0, 0);
    for (std::uint16_t i = 0; i < meshes.size(); ++i) {
      auto source = indices[i];
      const auto &mesh = meshes[i];
      w->setMeshName(i, mesh.name.c_str());
      std::vector<dna::Position> p;
      for (auto v : neutral[i])
        p.push_back({float(v[0] / rig.scale), float(v[1] / rig.scale),
                     float(v[2] / rig.scale)});
      w->setVertexPositions(i, p.data(), static_cast<std::uint32_t>(p.size()));
      std::vector<dna::TextureCoordinate> uv;
      auto us = r.getVertexTextureCoordinateUs(source),
           vs = r.getVertexTextureCoordinateVs(source);
      for (std::size_t v = 0; v < us.size(); ++v)
        uv.push_back({us[v], vs[v]});
      w->setVertexTextureCoordinates(i, uv.data(),
                                     static_cast<std::uint32_t>(uv.size()));
      std::vector<dna::Normal> normals;
      auto nx = r.getVertexNormalXs(source), ny = r.getVertexNormalYs(source),
           nz = r.getVertexNormalZs(source);
      for (std::size_t v = 0; v < nx.size(); ++v)
        normals.push_back({nx[v], ny[v], nz[v]});
      w->setVertexNormals(i, normals.data(),
                          static_cast<std::uint32_t>(normals.size()));
      std::vector<dna::VertexLayout> layouts;
      auto lp = r.getVertexLayoutPositionIndices(source),
           lt = r.getVertexLayoutTextureCoordinateIndices(source),
           ln = r.getVertexLayoutNormalIndices(source);
      for (std::size_t v = 0; v < lp.size(); ++v)
        layouts.push_back({lp[v], lt[v], ln[v]});
      w->setVertexLayouts(i, layouts.data(),
                          static_cast<std::uint32_t>(layouts.size()));
      for (std::uint32_t f = 0; f < r.getFaceCount(source); ++f) {
        auto a = r.getFaceVertexLayoutIndices(source, f);
        w->setFaceVertexLayoutIndices(i, f, a.data(),
                                      static_cast<std::uint32_t>(a.size()));
      }
      w->setMaximumInfluencePerVertex(i,
                                      r.getMaximumInfluencePerVertex(source));
      for (std::uint32_t v = 0; v < mesh.positions.size(); ++v) {
        auto weights = r.getSkinWeightsValues(source, v);
        auto joints = r.getSkinWeightsJointIndices(source, v);
        w->setSkinWeightsValues(i, v, weights.data(),
                                static_cast<std::uint16_t>(weights.size()));
        w->setSkinWeightsJointIndices(
            i, v, joints.data(), static_cast<std::uint16_t>(joints.size()));
      }
      for (std::uint16_t channel = 0; channel < 52; ++channel) {
        std::vector<std::uint32_t> vertices;
        std::vector<dna::Delta> deltas;
        for (std::uint32_t v = 0; v < targets[channel][i].size(); ++v) {
          auto d = targets[channel][i][v] * (1 / rig.scale);
          for (auto x : d)
            if (!std::isfinite(x))
              throw std::runtime_error("Non-finite baked DNA delta");
          if (d[0] != 0 || d[1] != 0 || d[2] != 0) {
            vertices.push_back(v);
            deltas.push_back({float(d[0]), float(d[1]), float(d[2])});
          }
        }
        w->setBlendShapeChannelIndex(i, channel, channel);
        w->setBlendShapeTargetVertexIndices(
            i, channel, vertices.data(),
            static_cast<std::uint32_t>(vertices.size()));
        w->setBlendShapeTargetDeltas(i, channel, deltas.data(),
                                     static_cast<std::uint32_t>(deltas.size()));
        w->setMeshBlendShapeChannelMapping(i * 52 + channel, i, channel);
      }
    }
    w->write();
    check_status("Baked DNA write failed");
  }
  double maximum = 0;
  {
    Rig actual(temporary);
    std::vector<Mesh> actual_meshes;
    for (auto i : actual.reader->getMeshIndicesForLOD(0))
      actual_meshes.push_back(read_mesh(*actual.reader, i));
    if (actual_meshes.size() != meshes.size() ||
        actual.raw_names.size() != 52 ||
        actual.reader->getBlendShapeChannelCount() != 52)
      throw std::runtime_error("Baked DNA controls or mesh order changed");
    for (std::size_t i = 0; i < meshes.size(); ++i)
      if (actual_meshes[i].name != meshes[i].name)
        throw std::runtime_error("Baked DNA mesh order changed");
    for (std::uint16_t i = 0; i < 52; ++i) {
      auto name = target_names[i];
      if (actual.reader->getBlendShapeChannelName(i).c_str() != name)
        throw std::runtime_error("Baked DNA channel changed");
      name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
      if (actual.raw_names[i] != "CTRL_expressions." + name)
        throw std::runtime_error("Baked DNA control changed");
    }
    std::mt19937 random(0);
    for (int pose = 0; pose < 56; ++pose) {
      std::array<double, 52> weights{};
      if (pose > 0 && pose <= 52)
        weights[pose - 1] = 1;
      else if (pose > 52)
        for (auto &w : weights)
          w = double(random()) / std::mt19937::max();
      Json controls = Json::object();
      for (int i = 0; i < 52; ++i)
        if (weights[i] != 0)
          controls[actual.raw_names[i]] = weights[i];
      auto evaluated = actual.evaluate(actual_meshes, controls, "raw");
      for (std::size_t m = 0; m < meshes.size(); ++m) {
        if (evaluated[m].size() != neutral[m].size())
          throw std::runtime_error("Baked DNA vertex count changed");
        for (std::size_t v = 0; v < neutral[m].size(); ++v) {
          auto expected = neutral[m][v];
          for (int i = 0; i < 52; ++i)
            expected = expected + targets[i][m][v] * weights[i];
          auto error = norm(evaluated[m][v] - expected);
          if (!std::isfinite(error) || error > 2e-6)
            throw std::runtime_error(
                "Baked DNA differs from baked geometry by " +
                std::to_string(error) + " metres");
          maximum = std::max(maximum, error);
        }
      }
    }
  }
  replace_file(temporary, path);
  return {{"verified", true},
          {"poses", 56},
          {"max_error_m", maximum},
          {"tolerance_m", 2e-6}};
}
// SHA-256 (FIPS 180-4). Stream input in blocks so reports do not duplicate DNA
// in memory.
std::string sha256(const fs::path &path) {
  constexpr std::uint32_t k[] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                    0xa54ff53a, 0x510e527f, 0x9b05688c,
                                    0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  auto block = [&](const unsigned char *bytes) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t(bytes[i * 4]) << 24) |
             (std::uint32_t(bytes[i * 4 + 1]) << 16) |
             (std::uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      auto a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3)) + w[i - 7] +
             (rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10));
    }
    auto v = h;
    for (int i = 0; i < 64; ++i) {
      auto t = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) +
               ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
      auto s = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) +
               ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
      for (int j = 7; j > 0; --j)
        v[j] = v[j - 1];
      v[4] += t;
      v[0] = t + s;
    }
    for (int i = 0; i < 8; ++i)
      h[i] += v[i];
  };
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("Cannot hash " + path.u8string());
  std::array<unsigned char, 64> bytes{};
  std::uint64_t size = 0;
  while (f.read(reinterpret_cast<char *>(bytes.data()), 64)) {
    block(bytes.data());
    size += 64;
  }
  if (!f.eof())
    throw std::runtime_error("Cannot read " + path.u8string());
  auto count = static_cast<std::size_t>(f.gcount());
  size += count;
  bytes[count++] = 0x80;
  if (count > 56) {
    std::fill(bytes.begin() + count, bytes.end(), 0);
    block(bytes.data());
    count = 0;
  }
  std::fill(bytes.begin() + count, bytes.end(), 0);
  auto bits = size * 8;
  for (int i = 0; i < 8; ++i)
    bytes[63 - i] = static_cast<unsigned char>(bits >> (8 * i));
  block(bytes.data());
  std::ostringstream out;
  for (auto x : h)
    out << std::hex << std::setfill('0') << std::setw(8) << x;
  return out.str();
}
} // namespace arkit
