// Probe: which joints carry nonzero RigLogic deltas under an expression pose?
// Usage: jd_debug2 <head.dna> <raw-control-name> <value> [more name value...]
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <riglogic/RigLogic.h>
#include <dna/BinaryStreamReader.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s head.dna ctrl val [ctrl val ...]\n", argv[0]);
    return 1;
  }
  rl4::ScopedPtr<rl4::FileStream> stream(rl4::makeScoped<rl4::FileStream>(
      argv[1], rl4::FileStream::AccessMode::Read,
      rl4::FileStream::OpenMode::Binary));
  rl4::ScopedPtr<rl4::BinaryStreamReader> reader(
      rl4::makeScoped<rl4::BinaryStreamReader>(
          stream.get(), []() {
            dna::Configuration c;
            c.minLOD = c.maxLOD = 0;
            return c;
          }()));
  reader->read();
  rl4::Configuration cfg;
  cfg.rotationType = rl4::RotationType::Quaternions;
  rl4::ScopedPtr<rl4::RigLogic> logic(
      rl4::makeScoped<rl4::RigLogic>(reader.get(), cfg));

  auto instance = rl4::makeScoped<rl4::RigInstance>(logic.get());
  instance->setLOD(0);
  for (int i = 2; i + 1 < argc; i += 2) {
    std::uint16_t idx = 0;
    bool found = false;
    for (std::uint16_t j = 0; j < reader->getRawControlCount(); ++j)
      if (std::string(reader->getRawControlName(j).c_str()) == argv[i]) {
        idx = j;
        found = true;
        break;
      }
    if (!found) {
      fprintf(stderr, "unknown control %s\n", argv[i]);
      return 2;
    }
    instance->setRawControl(idx, atof(argv[i + 1]));
  }
  logic->calculate(instance.get());
  auto out = instance->getJointOutputs();
  int moved = 0;
  for (std::uint16_t j = 0; j < reader->getJointCount(); ++j) {
    const float *d = out.data() + j * 10;
    double m = 0;
    for (int k = 0; k < 10; ++k)
      m += double(d[k]) * d[k];
    if (m > 1e-12) {
      ++moved;
      printf("%-16s |d|=%.5f  t=(%.4f,%.4f,%.4f) q=(%.4f,%.4f,%.4f,%.4f)\n",
             reader->getJointName(j).c_str(), std::sqrt(m), d[0], d[1], d[2],
             d[3], d[4], d[5], d[6]);
    }
  }
  printf("--- %d/%u joints with nonzero deltas\n", moved,
         unsigned(reader->getJointCount()));
  return 0;
}
