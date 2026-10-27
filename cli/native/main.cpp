#include "baker.h"
#include "config.h"
#include <iostream>
#ifdef _WIN32
// shellapi.h requires the Windows SDK types from windows.h first.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif
static int run(const std::vector<std::string> &args) {
  using namespace arkit;
  try {
    auto info = Json::parse(build_info_text);
    if (args.size() == 1) {
      if (args[0] == "--version") {
        std::cout << "mh-arkit-export " << info.at("version").get<std::string>()
                  << '\n';
        return 0;
      }
      if (args[0] == "--build-info") {
        std::cout << info.dump() << '\n';
        return 0;
      }
      if (args[0] == "--self-test") {
        auto test = self_test();
        test["version"] = info.at("version");
        std::cout << test.dump() << '\n';
        return 0;
      }
    }
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
      const auto &arg = args[i];
      auto value = [&]() {
        if (i + 1 == args.size())
          throw std::runtime_error("Missing value for " + arg);
        return args[++i];
      };
      if (arg == "--help" || arg == "-h") {
        std::cout
            << "Usage: mh-arkit-export head.dna -o output.glb|output.dna "
               "[options]\nBake 52 ARKit facial shapes from head DNA "
               "(LOD0).\n\nOptions:\n  -o, --output PATH   Output .glb or "
               ".dna\n  --body-dna PATH      Include neutral body geometry "
               "(GLB only)\n  --mapping PATH       JSON GUI/raw expression "
               "recipes\n  --mesh NAME          Select an LOD0 mesh "
               "(repeatable)\n  --version            Application version\n  "
               "--build-info         Build provenance as JSON\n  --self-test   "
               "       Verify synthetic DNA, skinning, GLB and baked DNA\n";
        return 0;
      }
      if (arg == "--output" || arg == "-o")
        o.output = fs::u8path(value());
      else if (arg == "--body-dna")
        o.body = fs::u8path(value());
      else if (arg == "--mapping")
        o.mapping = fs::u8path(value());
      else if (arg == "--mesh")
        o.meshes.push_back(value());
      else if (!arg.empty() && arg[0] == '-')
        throw std::runtime_error("Unknown option: " + arg);
      else if (o.input.empty())
        o.input = fs::u8path(arg);
      else
        throw std::runtime_error("Unexpected argument: " + arg);
    }
    if (o.input.empty() || o.output.empty())
      throw std::runtime_error("Usage: mh-arkit-export head.dna -o "
                               "output.glb|output.dna; see --help");
    bake(o);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << '\n';
    return 1;
  }
}
int main(int argc, char **argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  // MSVC's narrow argv uses the active code page; preserve Unicode paths
  // instead.
  int count = 0;
  auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide)
    return 1;
  for (int i = 1; i < count; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr,
                                nullptr);
    std::string arg(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), n, nullptr,
                        nullptr);
    arg.pop_back();
    args.push_back(arg);
  }
  LocalFree(wide);
#else
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);
#endif
  return run(args);
}
