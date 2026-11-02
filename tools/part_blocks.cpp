// part_blocks: CLI part vertex counts vs UE combined mesh; test whether UE
// verts align as contiguous blocks per part (DNA order).
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
  Buf c, u; if (!load_glb(argv[1], c) || !load_glb(argv[2], u)) return 2;
  std::vector<float> up = pos_of(u, (int)u.root.at("meshes").arr[0].find("primitives")->arr[0].find("attributes")->obj.at("POSITION").num);
  size_t un = up.size() / 3;
  size_t off = 0; bool ok = true;
  printf("%-20s %8s %8s %10s\n", "part", "cli_n", "ue_off", "mean|d|(mm)");
  for (const auto& m : c.root.at("meshes").arr) {
    const JV* nm = m.find("name"); std::string name = nm ? nm->str : "?";
    std::vector<float> cp = pos_of(c, (int)m.find("primitives")->arr[0].find("attributes")->obj.at("POSITION").num);
    size_t cn = cp.size() / 3;
    if (off + cn > un) { printf("%-20s %8zu %8zu  OVERFLOW\n", name.c_str(), cn, off); ok = false; break; }
    double s = 0;
    for (size_t i = 0; i < cn; i++) {
      double dx = cp[i*3] - up[(off+i)*3], dy = cp[i*3+1] - up[(off+i)*3+1], dz = cp[i*3+2] - up[(off+i)*3+2];
      s += std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    printf("%-20s %8zu %8zu %10.4f\n", name.c_str(), cn, off, s / cn * 1000);
    off += cn;
  }
  printf("total CLI %zu  UE %zu  %s\n", off, un, ok && off == un ? "EXACT BLOCK MATCH" : "no block match");
  return 0; }
