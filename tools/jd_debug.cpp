// glb_join_diff v2: nearest-neighbor vertex join (radius 1e-4 m) between CLI
// part meshes and the UE single mesh, then per-target delta comparison.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
struct JV; using JObj = std::map<std::string, JV>; using JArr = std::vector<JV>;
struct JV { enum Type { NUL, NUM, STR, ARR, OBJ } type = NUL; double num = 0; std::string str; JArr arr; JObj obj;
  const JV* find(const std::string& k) const { if (type != OBJ) return nullptr; auto it = obj.find(k); return it == obj.end() ? nullptr : &it->second; } };
static const char* jp(const char* p, std::string* s) { while (*p && *p != '"') { if (*p == '\\') p++; *s += *p++; } return p + 1; }
static const char* jvalue(const char* p, JV* out) {
  while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
  if (*p == '{') { out->type = JV::OBJ; p++;
    while (*p && *p != '}') { while (*p && *p != '"') p++; if (!*p) return p;
      JV key; key.type = JV::STR; p = jp(p + 1, &key.str); while (*p != ':') p++; p++;
      JV val; p = jvalue(p, &val); out->obj.emplace(key.str, std::move(val));
      while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++; } return p + 1; }
  if (*p == '[') { out->type = JV::ARR; p++;
    while (*p && *p != ']') { JV val; p = jvalue(p, &val); out->arr.push_back(std::move(val));
      while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++; } return p + 1; }
  if (*p == '"') { out->type = JV::STR; return jp(p + 1, &out->str); }
  char* e = nullptr; out->type = JV::NUM; out->num = strtod(p, &e); return e ? e : p; }
struct Buf { std::vector<char> bin; JObj root; };
static bool load_glb(const char* path, Buf& b) { std::ifstream f(path, std::ios::binary); if (!f) return false;
  std::vector<char> data((std::istreambuf_iterator<char>(f)), {}); JV json; size_t off = 12;
  while (off + 8 <= data.size()) { uint32_t cl, ct; memcpy(&cl, &data[off], 4); memcpy(&ct, &data[off + 4], 4);
    const char* body = data.data() + off + 8;
    if (ct == 0x4E4F534A) jvalue(body, &json); else if (ct == 0x004E4942) b.bin.assign(body, body + cl);
    off += 8 + cl; } b.root = std::move(json.obj); return true; }

struct Tgt { std::string name; std::vector<float> d; };
struct Prim { std::vector<float> neutral; std::vector<Tgt> targets; size_t nv = 0; };

static std::vector<float> readf(const Buf& b, const JV& acc) {
  const JArr& views = b.root.at("bufferViews").arr;
  int vi = (int)acc.find("bufferView")->num;
  int bo = acc.find("byteOffset") ? (int)acc.find("byteOffset")->num : 0;
  int ct = (int)acc.find("count")->num; int comp = (int)acc.find("componentType")->num;
  const JV& v = views[vi]; int vo = v.find("byteOffset") ? (int)v.find("byteOffset")->num : 0;
  const char* base = b.bin.data() + vo + bo;
  std::vector<float> o(ct * 3);
  if (comp == 5126) memcpy(o.data(), base, ct * 12);
  else if (comp == 5123) for (int i = 0; i < ct * 3; i++) { int16_t x; memcpy(&x, base + i * 2, 2); o[i] = x / 32767.0f; }
  else if (comp == 5125) for (int i = 0; i < ct * 3; i++) { uint8_t x = base[i]; o[i] = x / 255.0f; }
  return o; }

static Prim prim_of(const Buf& b, const JV& mesh) {
  Prim p; const JV& prim = mesh.find("primitives")->arr[0];
  const JArr& accs = b.root.at("accessors").arr;
  p.neutral = readf(b, accs[(int)prim.find("attributes")->obj.at("POSITION").num]);
  p.nv = p.neutral.size() / 3;
  std::vector<std::string> names;
  const JV* ex = prim.find("extras"); const JV* tn = ex ? ex->find("targetNames") : nullptr;
  if (tn) for (auto& n : tn->arr) names.push_back(n.str);
  if (names.empty()) { const JV* mex = mesh.find("extras"); const JV* mtn = mex ? mex->find("targetNames") : nullptr;
    if (mtn) for (auto& n : mtn->arr) names.push_back(n.str); }
  const JV* tg = prim.find("targets");
  if (tg) for (size_t i = 0; i < tg->arr.size(); i++) { const JV* pos = tg->arr[i].find("POSITION"); if (!pos) continue;
    p.targets.push_back({ i < names.size() ? names[i] : "t" + std::to_string(i), readf(b, accs[(int)pos->num]) }); }
  return p; }

// simple spatial hash on 1mm cells for NN join
struct Grid { double cell = 1e-3; std::map<std::tuple<int,int,int>, std::vector<int>> cells;
  void add(const std::vector<float>& pos, int i) {
    int x = (int)std::floor(pos[i*3]/cell), y = (int)std::floor(pos[i*3+1]/cell), z = (int)std::floor(pos[i*3+2]/cell);
    cells[{x,y,z}].push_back(i); }
  int nearest(const std::vector<float>& pos, int i, double maxd) const {
    int bx = (int)std::floor(pos[i*3]/cell), by = (int)std::floor(pos[i*3+1]/cell), bz = (int)std::floor(pos[i*3+2]/cell);
    int r = (int)std::ceil(maxd/cell); int best = -1; double bd = maxd*maxd;
    for (int dx=-r;dx<=r;dx++) for (int dy=-r;dy<=r;dy++) for (int dz=-r;dz<=r;dz++) {
      auto it = cells.find({bx+dx,by+dy,bz+dz}); if (it==cells.end()) continue;
      for (int j : it->second) { double ax=pos[i*3]-pos[j*3],ay=pos[i*3+1]-pos[j*3+1],az=pos[i*3+2]-pos[j*3+2];
        double d2=ax*ax+ay*ay+az*az; if (d2<bd) { bd=d2; best=j; } } }
    return best; } };

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: glb_join_diff cli.glb ue.glb\n"); return 1; }
  Buf cb, ub; if (!load_glb(argv[1], cb) || !load_glb(argv[2], ub)) { fprintf(stderr, "load\n"); return 2; }
  Prim ue = prim_of(ub, ub.root.at("meshes").arr[0]);
  Grid g; for (size_t i = 0; i < ue.nv; i++) g.add(ue.neutral, (int)i);
  bool usedUE = false; std::vector<char> taken(ue.nv, 0); (void)usedUE;

  printf("%-18s %-22s %7s %10s %10s %10s %10s %8s\n", "part", "target", "n", "maxUE(mm)", "maxCLI(mm)", "rmsUE", "rmsd", "corr");
  double worst = 0; std::string worsts; double sum_rmsd = 0; int n_rows = 0;
  for (const auto& m : cb.root.at("meshes").arr) {
    Prim cp = prim_of(cb, m);
    int blocked = 0; int nfound = 0;
    const JV* nm = m.find("name"); std::string name = nm ? nm->str : "?";
    std::vector<int> match(cp.nv, -1); int nm2 = 0;
    for (size_t i = 0; i < cp.nv; i++) {
      int j = g.nearest(cp.neutral, (int)i, 6e-3); if (j >= 0) nfound++;
      if (j >= 0) { if (!taken[j]) { match[i] = j; taken[j] = 1; nm2++; } else blocked++; }
    }
    if (!nm2) { printf("%-18s (no match: found=%d blocked=%d of %zu)\n", name.c_str(), nfound, blocked, cp.nv); continue; }
    fprintf(stderr, "[%s] matched=%d found=%d blocked=%d of %zu\n", name.c_str(), nm2, nfound, blocked, cp.nv);
    for (auto& t : cp.targets) {
      std::string swapped = t.name;
      bool sw = false;
      if (swapped.find("Left") != std::string::npos) { swapped.replace(swapped.find("Left"), 4, "Right"); sw = true; }
      else if (swapped.find("Right") != std::string::npos) { swapped.replace(swapped.find("Right"), 5, "Left"); sw = true; }
      const std::vector<float>* ud = nullptr;
      for (auto& u : ue.targets) if (u.name == (sw ? swapped : t.name)) { ud = &u.d; break; }
      if (!ud) for (auto& u : ue.targets) if (u.name == t.name) { ud = &u.d; break; }
      if (!ud) { printf("%-18s %-22s not in UE\n", name.c_str(), t.name.c_str()); continue; }
      double sd = 0, ssa = 0, ssb = 0, dot = 0, maxU = 0, maxC = 0; int n = 0;
      for (size_t i = 0; i < cp.nv; i++) { int j = match[i]; if (j < 0) continue;
        double ax = (*ud)[j*3], ay = (*ud)[j*3+1], az = (*ud)[j*3+2];
        double bx = t.d[i*3], by = t.d[i*3+1], bz = t.d[i*3+2];
        maxU = std::max(maxU, std::sqrt(ax*ax+ay*ay+az*az)); maxC = std::max(maxC, std::sqrt(bx*bx+by*by+bz*bz));
        double dx = ax-bx, dy = ay-by, dz = az-bz;
        sd += dx*dx+dy*dy+dz*dz; ssa += ax*ax+ay*ay+az*az; ssb += bx*bx+by*by+bz*bz;
        dot += ax*bx+ay*by+az*bz; n++; }
      if (!n) continue;
      double rmsd = std::sqrt(sd/n)*1000, corr = ssa>0&&ssb>0 ? dot/std::sqrt(ssa*ssb) : 0;
      printf("%-18s %-22s %7d %10.3f %10.3f %10.3f %10.3f %8.4f\n",
             name.c_str(), t.name.c_str(), n, maxU*1000, maxC*1000, std::sqrt(ssa/n)*1000, rmsd, corr);
      worst = std::max(worst, rmsd); if (rmsd == worst) worsts = name + "/" + t.name;
      sum_rmsd += rmsd; n_rows++;
    }
  }
  printf("\nworst rmsd: %.3f mm | mean rmsd: %.3f mm over %d rows\n", worst, sum_rmsd/std::max(n_rows,1), n_rows);
  return 0; }
