// glb_diff: compare morph targets between two ARKit GLBs (UE-plugin vs CLI).
// Matches meshes by name, targets by morph-name extension data, reports
// per-target max/RMS displacement deltas over the union of vertices.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// ---- minimal JSON value (subset) ----
struct JV;
using JObj = std::map<std::string, JV>;
using JArr = std::vector<JV>;
struct JV {
  enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
  double num = 0; bool b = false;
  std::string str; JArr arr; JObj obj;
  const JV* find(const std::string& k) const {
    if (type != OBJ) return nullptr;
    auto it = obj.find(k); return it == obj.end() ? nullptr : &it->second;
  }
};
static const char* jp(const char* p, std::string* out);
static const char* jvalue(const char* p, JV* out) {
  while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
  if (*p == '{') {
    out->type = JV::OBJ; p++;
    while (*p && *p != '}') {
      while (*p && *p != '"') p++;
      if (!*p) return p;
      JV key; key.type = JV::STR; p = jp(p + 1, &key.str);
      while (*p != ':') p++; p++;
      JV val; p = jvalue(p, &val);
      out->obj.emplace(key.str, std::move(val));
      while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
    }
    return p + 1;
  }
  if (*p == '[') {
    out->type = JV::ARR; p++;
    while (*p && *p != ']') {
      JV val; p = jvalue(p, &val);
      out->arr.push_back(std::move(val));
      while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
    }
    return p + 1;
  }
  if (*p == '"') { out->type = JV::STR; return jp(p + 1, &out->str); }
  char* end = nullptr;
  out->type = JV::NUM; out->num = strtod(p, &end);
  return end ? end : p;
}
static const char* jp(const char* p, std::string* s) {
  while (*p && *p != '"') { if (*p == '\\') p++; *s += *p++; }
  return p + 1;
}

// ---- GLB ----
struct Accessor { int bvView = -1, bvOffset = 0; int componentType = 0; int count = 0; std::string type; };
struct GLB {
  std::vector<char> bin;
  JArr meshes, nodes, accessorsRaw, bufferViewsRaw, extensionsRaw;
};

static bool load_glb(const char* path, JV* json, std::vector<char>* bin) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { fprintf(stderr, "open %s\n", path); return false; }
  std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
  uint32_t len = 12;
  while (len + 8 <= data.size()) {
    uint32_t clen, ctype; memcpy(&clen, &data[len], 4); memcpy(&ctype, &data[len+4], 4);
    const char* body = data.data() + len + 8;
    if (ctype == 0x4E4F534A) jvalue(body, json);
    else if (ctype == 0x004E4942) bin->assign(body, body + clen);
    len += 8 + clen;
  }
  return json->type == JV::OBJ;
}

struct Target { std::string name; std::vector<float> pos; };  // xyz floats
struct MeshTargets { std::string mesh; std::vector<Target> targets; };

static std::vector<MeshTargets> extract(const char* path) {
  JV json; std::vector<char> bin;
  if (!load_glb(path, &json, &bin)) { fprintf(stderr, "bad glb %s\n", path); exit(2); }
  const JArr& meshes = json.obj.at("meshes").arr;
  const JArr& nodes = json.obj.at("nodes").arr;
  const JArr& accessors = json.obj.at("accessors").arr;
  const JArr& accessorsV = accessors;
  const JArr& views = json.obj.at("bufferViews").arr;
  // mesh name -> node name mapping via scene traversal is skipped: match mesh name directly
  std::vector<MeshTargets> out;
  for (const auto& m : meshes) {
    MeshTargets mt;
    const JV* nm = m.find("name");
    mt.mesh = nm ? nm->str : "mesh" + std::to_string(out.size());
    const JV* prims = m.find("primitives");
    if (!prims || prims->arr.empty()) continue;
    const JV* targets = prims->arr[0].find("targets");
    if (!targets) continue;
    // morph target names: EXT_mesh_gpu_instancing? no: KHR_mesh_quantization? The
    // standard location is mesh.extras.targetNames (CLI writes it) and
    // mesh.primitives[0].extras.targetNames; UE GLB may differ -> search extras.
    std::vector<std::string> names;
    for (const char* spot : {"extras"}) {
      const JV* ex = prims->arr[0].find(spot);
      const JV* tn = ex ? ex->find("targetNames") : nullptr;
      if (tn) for (const auto& n : tn->arr) names.push_back(n.str);
    }
    if (names.empty()) {
      const JV* ex = m.find("extras");
      const JV* tn = ex ? ex->find("targetNames") : nullptr;
      if (tn) for (const auto& n : tn->arr) names.push_back(n.str);
    }
    int ti = 0;
    for (const auto& t : targets->arr) {
      const JV* pos = t.find("POSITION");
      if (!pos) continue;
      const JV& acc = accessorsV[(int)pos->num];
      int vi = (int)acc.find("bufferView")->num;
      int byteOffset = acc.find("byteOffset") ? (int)acc.find("byteOffset")->num : 0;
      int count = (int)acc.find("count")->num;
      int comp = (int)acc.find("componentType")->num;
      size_t sz = comp == 5126 ? 4 : 2;
      const JV& view = views[vi];
      int voff = view.find("byteOffset") ? (int)view.find("byteOffset")->num : 0;
      const char* base = bin.data() + voff + byteOffset;
      Target tg;
      tg.name = ti < (int)names.size() ? names[ti] : ("target" + std::to_string(ti));
      tg.pos.resize(count * 3);
      if (comp == 5126) {
        memcpy(tg.pos.data(), base, count * 3 * 4);
      } else { // normalized shorts
        for (int i = 0; i < count * 3; i++) {
          int16_t v; memcpy(&v, base + i * 2, 2);
          tg.pos[i] = std::max(v, (int16_t)-32768) / 32767.0f;
        }
      }
      mt.targets.push_back(std::move(tg));
      ti++;
    }
    out.push_back(std::move(mt));
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: glb_diff a.glb b.glb\n"); return 1; }
  auto A = extract(argv[1]);
  auto B = extract(argv[2]);
  std::map<std::string, MeshTargets*> bm;
  for (auto& m : B) bm[m.mesh] = &m;
  printf("%-14s %-22s %10s %10s %10s %10s %8s\n", "mesh", "target", "maxA(mm)", "maxB(mm)", "rmsAB(mm)", "rmsd(mm)", "corr");
  double worst = 0; std::string worsts;
  for (auto& a : A) {
    auto it = bm.find(a.mesh);
    if (it == bm.end()) { printf("%-14s -- not in B\n", a.mesh.c_str()); continue; }
    auto& b = *it->second;
    std::map<std::string, Target*> bt;
    for (auto& t : b.targets) bt[t.name] = &t;
    for (auto& at : a.targets) {
      auto bit = bt.find(at.name);
      if (bit == bt.end()) { printf("%-14s %-22s only in A\n", a.mesh.c_str(), at.name.c_str()); continue; }
      auto& btg = *bit->second;
      size_t n = std::min(at.pos.size(), btg.pos.size());
      double maxA = 0, maxB = 0, ss = 0, ssa = 0, ssb = 0, dot = 0;
      for (size_t i = 0; i < n; i += 3) {
        double ax = at.pos[i], ay = at.pos[i+1], az = at.pos[i+2];
        double bx = btg.pos[i], by = btg.pos[i+1], bz = btg.pos[i+2];
        double na = std::sqrt(ax*ax+ay*ay+az*az), nb = std::sqrt(bx*bx+by*by+bz*bz);
        maxA = std::max(maxA, na); maxB = std::max(maxB, nb);
        double dx = ax-bx, dy = ay-by, dz = az-bz;
        ss += dx*dx+dy*dy+dz*dz;
        ssa += ax*ax+ay*ay+az*az; ssb += bx*bx+by*by+bz*bz;
        dot += ax*bx+ay*by+az*bz;
      }
      size_t nv = n / 3;
      double rmsd = std::sqrt(ss / nv) * 1000;
      double rmsA = std::sqrt(ssa / nv) * 1000;
      double corr = ssa > 0 && ssb > 0 ? dot / std::sqrt(ssa * ssb) : 0;
      printf("%-14s %-22s %10.3f %10.3f %10.3f %10.3f %8.4f\n",
             a.mesh.c_str(), at.name.c_str(), maxA*1000, maxB*1000, rmsA, rmsd, corr);
      if (rmsd > worst) { worst = rmsd; worsts = a.mesh + "/" + at.name; }
    }
  }
  printf("\nworst rmsd: %.3f mm (%s)\n", worst, worsts.c_str());
  return 0;
}
