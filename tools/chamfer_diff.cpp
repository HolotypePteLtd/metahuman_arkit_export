// chamfer_diff: per-target Chamfer comparison of morph displacement fields
// between CLI (9 part meshes) and UE (single mesh) GLBs. Works despite
// different vertex orders. For each CLI vertex with displacement, find nearest
// UE vertex (by neutral position) and compare its displacement; and vice versa.
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
  while (*p == ' ' || *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
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
struct Prim { std::string name; std::vector<float> neutral; std::vector<Tgt> targets; size_t nv = 0; };
static std::vector<float> readf(const Buf& b, const JV& acc) {
  const JArr& views = b.root.at("bufferViews").arr;
  int vi = (int)acc.find("bufferView")->num;
  int bo = acc.find("byteOffset") ? (int)acc.find("byteOffset")->num : 0;
  int ct = (int)acc.find("count")->num; int comp = (int)acc.find("componentType")->num;
  const JV& v = views[vi]; int vo = v.find("byteOffset") ? (int)v.find("byteOffset")->num : 0;
  const char* base = b.bin.data() + vo + bo;
  std::vector<float> o(ct * 3);
  if (comp == 5126) memcpy(o.data(), base, ct * 12);
  return o; }
static Prim prim_of(const Buf& b, const JV& mesh) {
  Prim p; const JV* nm = mesh.find("name"); p.name = nm ? nm->str : "?";
  const JV& prim = mesh.find("primitives")->arr[0];
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
struct Grid {
  double cell = 2e-3;
  const std::vector<float>* ref = nullptr;
  std::map<std::tuple<int,int,int>, std::vector<int>> cells;
  void build(const std::vector<float>* pos) {
    ref = pos;
    for (size_t i = 0; i < ref->size() / 3; i++)
      cells[{(int)std::floor((*ref)[i*3]/cell),(int)std::floor((*ref)[i*3+1]/cell),(int)std::floor((*ref)[i*3+2]/cell)}].push_back((int)i);
  }
  int nearest(const std::vector<float>& q, int i, double maxd, double* outd = nullptr) const {
    int bx=(int)std::floor(q[i*3]/cell), by=(int)std::floor(q[i*3+1]/cell), bz=(int)std::floor(q[i*3+2]/cell);
    int r=(int)std::ceil(maxd/cell); int best=-1; double bd=maxd*maxd;
    for (int dx=-r;dx<=r;dx++) for (int dy=-r;dy<=r;dy++) for (int dz=-r;dz<=r;dz++) {
      auto it = cells.find({bx+dx,by+dy,bz+dz}); if (it==cells.end()) continue;
      for (int j : it->second) { double ax=q[i*3]-(*ref)[j*3],ay=q[i*3+1]-(*ref)[j*3+1],az=q[i*3+2]-(*ref)[j*3+2];
        double d2=ax*ax+ay*ay+az*az; if (d2<bd) { bd=d2; best=j; } } }
    if (outd && best >= 0) *outd = std::sqrt(bd);
    return best; }
};
int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: chamfer_diff cli.glb ue.glb\n"); return 1; }
  Buf cb, ub; if (!load_glb(argv[1], cb) || !load_glb(argv[2], ub)) return 2;
  std::vector<Prim> cps;
  for (const auto& m : cb.root.at("meshes").arr) cps.push_back(prim_of(cb, m));
  Prim ue = prim_of(ub, ub.root.at("meshes").arr[0]);

  // flatten CLI points across parts
  std::vector<float> cpos; std::vector<int> cpart;
  for (size_t p = 0; p < cps.size(); p++) {
    cpos.insert(cpos.end(), cps[p].neutral.begin(), cps[p].neutral.end());
    cpart.insert(cpart.end(), cps[p].nv, (int)p);
  }
  Grid ug; ug.build(&ue.neutral);
  Grid cg; cg.build(&cpos);
  const double R = 3e-3;  // neutral-pairing radius

  // for each target name: bidirectional displacement comparison at NN pairs
  printf("%-22s %8s %10s %10s %10s %8s\n", "target", "npairs", "rmsd(mm)", "p95(mm)", "maxUE(mm)", "corr");
  double worst = 0; std::string worsts;
  for (const auto& utRaw : ue.targets) {
    // The UE plugin swaps Left/Right in exported morph names (documented quirk
    // in MHARKitExportBPLibrary.cpp); undo it. Skip the Pose_4..17 tail.
    std::string name = utRaw.name;
    if (name.rfind("Pose_", 0) == 0) continue;
    if (argc > 3) if (name.find("Left") != std::string::npos) name.replace(name.find("Left"), 4, "Right");
    else if (argc > 3 && name.find("Right") != std::string::npos) name.replace(name.find("Right"), 5, "Left");
    const Tgt* ct = nullptr; int cpIdx = -1;
    const Tgt* ut = &utRaw;
    for (size_t p = 0; p < cps.size() && !ct; p++)
      for (const auto& t : cps[p].targets) if (t.name == name) { ct = &t; cpIdx = (int)p; break; }
    if (!ct) { printf("%-22s not in CLI\n", name.c_str()); continue; }
    bool bad = false;
    for (float v : ut->d) if (!std::isfinite(v)) bad = true;
    for (float v : ct->d) if (!std::isfinite(v)) bad = true;
    if (bad) { printf("%-22s NaN/inf in deltas\n", name.c_str()); continue; }
    double sd = 0, ssa = 0, ssb = 0, dot = 0; long n = 0;
    std::vector<double> ds;
    double maxU = 0;
    for (size_t i = 0; i < cpos.size() / 3; i++) {
      double nd;
      int j = ug.nearest(cpos, (int)i, R, &nd);
      if (j < 0 || nd > R) continue;
      double ax = ut->d[j*3], ay = ut->d[j*3+1], az = ut->d[j*3+2];
      double bx = ct->d[i*3], by = ct->d[i*3+1], bz = ct->d[i*3+2];
      maxU = std::max(maxU, std::sqrt(ax*ax+ay*ay+az*az));
      double dx = ax-bx, dy = ay-by, dz = az-bz;
      double d2 = dx*dx+dy*dy+dz*dz;
      sd += d2; ds.push_back(std::sqrt(d2));
      ssa += ax*ax+ay*ay+az*az; ssb += bx*bx+by*by+bz*bz; dot += ax*bx+ay*by+az*bz; n++;
    }
    if (n < 100) { printf("%-22s %8ld too few pairs\n", ut->name.c_str(), n); continue; }
    std::sort(ds.begin(), ds.end());
    double rmsd = std::sqrt(sd/n)*1000;
    double corr = ssa>0&&ssb>0 ? dot/std::sqrt(ssa*ssb) : 0;
    printf("%-22s %8ld %10.3f %10.3f %10.3f %8.4f\n", ut->name.c_str(), n, rmsd, ds[(size_t)(0.95*(ds.size()-1))]*1000, maxU*1000, corr);
    if (rmsd > worst) { worst = rmsd; worsts = ut->name; }
  }
  printf("\nworst rmsd: %.3f mm (%s)\n", worst, worsts.c_str());
  return 0;
}
