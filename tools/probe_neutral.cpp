// probe_neutral: compare neutral vertex sets of the two GLBs.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
struct JV; using JObj = std::map<std::string, JV>; using JArr = std::vector<JV>;
struct JV { enum Type { NUL, NUM, STR, ARR, OBJ } type = NUL; double num=0; std::string str; JArr arr; JObj obj;
  const JV* find(const std::string& k) const { if (type!=OBJ) return nullptr; auto it=obj.find(k); return it==obj.end()?nullptr:&it->second; } };
static const char* jp(const char* p, std::string* s){ while(*p&&*p!='"'){ if(*p=='\\')p++; *s+=*p++; } return p+1; }
static const char* jvalue(const char* p, JV* out){
  while(*p==' '||*p=='\n'||*p=='\t'||*p=='\r')p++;
  if(*p=='{'){ out->type=JV::OBJ; p++;
    while(*p&&*p!='}'){ while(*p&&*p!='"')p++; if(!*p)return p;
      JV key; key.type=JV::STR; p=jp(p+1,&key.str); while(*p!=':')p++; p++;
      JV val; p=jvalue(p,&val); out->obj.emplace(key.str,std::move(val));
      while(*p==' '||*p=='\n'||*p=='\t'||*p==',')p++; } return p+1; }
  if(*p=='['){ out->type=JV::ARR; p++;
    while(*p&&*p!=']'){ JV val; p=jvalue(p,&val); out->arr.push_back(std::move(val));
      while(*p==' '||*p=='\n'||*p=='\t'||*p==',')p++; } return p+1; }
  if(*p=='"'){ out->type=JV::STR; return jp(p+1,&out->str); }
  char* e=nullptr; out->type=JV::NUM; out->num=strtod(p,&e); return e?e:p; }
struct Buf { std::vector<char> bin; JObj root; };
static bool load_glb(const char* path, Buf& b){ std::ifstream f(path,std::ios::binary); if(!f)return false;
  std::vector<char> data((std::istreambuf_iterator<char>(f)),{}); JV json; size_t off=12;
  while(off+8<=data.size()){ uint32_t cl,ct; memcpy(&cl,&data[off],4); memcpy(&ct,&data[off+4],4);
    const char* body=data.data()+off+8;
    if(ct==0x4E4F534A) jvalue(body,&json); else if(ct==0x004E4942) b.bin.assign(body,body+cl);
    off+=8+cl; } b.root=std::move(json.obj); return true; }
static std::vector<float> pos_of(const Buf& b,int accIdx,bool* isShort=nullptr){
  const JArr& accs=b.root.at("accessors").arr; const JArr& views=b.root.at("bufferViews").arr;
  const JV& a=accs[accIdx]; int vi=(int)a.find("bufferView")->num;
  int bo=a.find("byteOffset")?(int)a.find("byteOffset")->num:0;
  int ct=(int)a.find("count")->num; int comp=(int)a.find("componentType")->num;
  if(isShort)*isShort=(comp==5123);
  const JV& v=views[vi]; int vo=v.find("byteOffset")?(int)v.find("byteOffset")->num:0;
  const char* base=b.bin.data()+vo+bo; std::vector<float> o(ct*3);
  if(comp==5126) memcpy(o.data(),base,ct*12);
  return o; }
int main(int argc,char**argv){
  if(argc!=3){fprintf(stderr,"probe a.glb b.glb\n");return 1;}
  Buf a,b; load_glb(argv[1],a); load_glb(argv[2],b);
  const JArr& am=a.root.at("meshes").arr; const JArr& bm=b.root.at("meshes").arr;
  printf("A meshes: %zu  B meshes: %zu\n", am.size(), bm.size());
  const JV& ap=am[0].find("primitives")->arr[0];
  bool sh=false; std::vector<float> an=pos_of(a,(int)ap.find("attributes")->obj.at("POSITION").num,&sh);
  printf("A[0] verts=%zu short=%d  first5:\n",an.size()/3,(int)sh);
  for(int i=0;i<5;i++)printf("  %.6f %.6f %.6f\n",an[i*3],an[i*3+1],an[i*3+2]);
  const JV& bp=bm[0].find("primitives")->arr[0];
  bool sh2=false; std::vector<float> bn=pos_of(b,(int)bp.find("attributes")->obj.at("POSITION").num,&sh2);
  printf("B[0] verts=%zu short=%d  first5:\n",bn.size()/3,(int)sh2);
  for(int i=0;i<5;i++)printf("  %.6f %.6f %.6f\n",bn[i*3],bn[i*3+1],bn[i*3+2]);
  // bbox
  double amin[3]={1e9,1e9,1e9},amax[3]={-1e9,-1e9,-1e9},bmin[3]={1e9,1e9,1e9},bmax[3]={-1e9,-1e9,-1e9};
  for(size_t i=0;i<an.size()/3;i++)for(int k=0;k<3;k++){amin[k]=std::min(amin[k],(double)an[i*3+k]);amax[k]=std::max(amax[k],(double)an[i*3+k]);}
  for(size_t i=0;i<bn.size()/3;i++)for(int k=0;k<3;k++){bmin[k]=std::min(bmin[k],(double)bn[i*3+k]);bmax[k]=std::max(bmax[k],(double)bn[i*3+k]);}
  printf("A bbox min %.4f %.4f %.4f max %.4f %.4f %.4f\n",amin[0],amin[1],amin[2],amax[0],amax[1],amax[2]);
  printf("B bbox min %.4f %.4f %.4f max %.4f %.4f %.4f\n",bmin[0],bmin[1],bmin[2],bmax[0],bmax[1],bmax[2]);
  return 0; }
