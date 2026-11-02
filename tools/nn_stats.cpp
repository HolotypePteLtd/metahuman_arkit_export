// nn_stats: distribution of nearest-neighbor distances between CLI and UE neutrals.
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
static std::vector<float> pos_of(const Buf& b, int accIdx) {
  const JArr& accs = b.root.at("accessors").arr; const JArr& views = b.root.at("bufferViews").arr;
  const JV& a = accs[accIdx]; int vi = (int)a.find("bufferView")->num;
  int bo = a.find("byteOffset") ? (int)a.find("byteOffset")->num : 0;
  int ct = (int)a.find("count")->num;
  const JV& v = views[vi]; int vo = v.find("byteOffset") ? (int)v.find("byteOffset")->num : 0;
  const char* base = b.bin.data() + vo + bo; std::vector<float> o(ct * 3);
  memcpy(o.data(), base, ct * 12); return o; }
int main(int argc, char** argv) {
  Buf a, b; if (!load_glb(argv[1], a) || !load_glb(argv[2], b)) return 2;
  const JV& am = a.root.at("meshes").arr[0];
  std::vector<float> ap = pos_of(a, (int)am.find("primitives")->arr[0].find("attributes")->obj.at("POSITION").num);
  const JV& bm = b.root.at("meshes").arr[0];
  std::vector<float> bp = pos_of(b, (int)bm.find("primitives")->arr[0].find("attributes")->obj.at("POSITION").num);
  size_t na = ap.size() / 3, nb = bp.size() / 3;
  // grid for B
  const double cell = 0.01; std::map<std::tuple<int,int,int>, std::vector<int>> grid;
  for (size_t j = 0; j < nb; j++)
    grid[{(int)std::floor(bp[j*3]/cell),(int)std::floor(bp[j*3+1]/cell),(int)std::floor(bp[j*3+2]/cell)}].push_back((int)j);
  std::vector<double> ds;
  for (size_t i = 0; i < na; i++) {
    int bx=(int)std::floor(ap[i*3]/cell), by=(int)std::floor(ap[i*3+1]/cell), bz=(int)std::floor(ap[i*3+2]/cell);
    double bd = 1e18;
    for (int dx=-1;dx<=1;dx++) for (int dy=-1;dy<=1;dy++) for (int dz=-1;dz<=1;dz++) {
      auto it = grid.find({bx+dx,by+dy,bz+dz}); if (it == grid.end()) continue;
      for (int j : it->second) { double x=ap[i*3]-bp[j*3], y=ap[i*3+1]-bp[j*3+1], z=ap[i*3+2]-bp[j*3+2];
        double d = x*x+y*y+z*z; if (d < bd) bd = d; } }
    if (bd < 1e18) ds.push_back(std::sqrt(bd));
  }
  std::sort(ds.begin(), ds.end());
  auto q = [&](double f){ return ds[(size_t)(f*(ds.size()-1))]; };
  printf("matched %zu / %zu  min=%.2e p10=%.2e p50=%.2e p90=%.2e p99=%.2e max=%.2e (m)\n",
         ds.size(), na, q(0.0), q(0.10), q(0.5), q(0.9), q(0.99), q(1.0));
  // directional offset stats for close pairs
  double sx=0, sy=0, sz=0; int n=0;
  for (size_t i = 0; i < na; i++) {
    int bx=(int)std::floor(ap[i*3]/cell), by=(int)std::floor(ap[i*3+1]/cell), bz=(int)std::floor(ap[i*3+2]/cell);
    double bd = 1e18; int bj=-1;
    for (int dx=-1;dx<=1;dx++) for (int dy=-1;dy<=1;dy++) for (int dz=-1;dz<=1;dz++) {
      auto it = grid.find({bx+dx,by+dy,bz+dz}); if (it == grid.end()) continue;
      for (int j : it->second) { double x=ap[i*3]-bp[j*3], y=ap[i*3+1]-bp[j*3+1], z=ap[i*3+2]-bp[j*3+2];
        double d = x*x+y*y+z*z; if (d < bd) { bd = d; bj = j; } } }
    if (bj >= 0 && bd < 1e-8) { sx += ap[i*3]-bp[bj*3]; sy += ap[i*3+1]-bp[bj*3+1]; sz += ap[i*3+2]-bp[bj*3+2]; n++; }
  }
  if (n) printf("close pairs %d  mean offset (%.2e, %.2e, %.2e)\n", n, sx/n, sy/n, sz/n);
  return 0; }
