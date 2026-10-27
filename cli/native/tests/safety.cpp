#include "baker.h"
#include "process.h"
#include <functional>
#include <iostream>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

namespace {
using namespace arkit;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action, const std::string &message) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, message);
}
void text_file(const fs::path &path, const std::string &contents) {
  std::ofstream f(path, std::ios::binary);
  f << contents;
  f.close();
  require(bool(f), "Cannot prepare test file");
}
struct Fixture {
  TemporaryDirectory root{fs::temp_directory_path()};
  Options options;
  fs::path report;
  Fixture() {
    options.input = root.path / "head.dna";
    options.mapping = root.path / "mapping.json";
    options.output = root.path / "output.glb";
    report = root.path / "output.report.json";
    write_fixture(options.input);
    Json mapping = {{"control_space", "raw"}, {"targets", Json::object()}};
    for (const auto &name : target_names)
      mapping["targets"][name] = {
          {"controls", {{"CTRL_expressions.test", 1.0}}}};
    write_json(options.mapping, mapping);
  }
  void previous_outputs() {
    text_file(options.output, "previous export");
    text_file(report, "previous report");
  }
};
void malformed_dna(const fs::path &binary) {
  Fixture f;
  f.previous_outputs();
  const auto old_output = sha256(f.options.output),
             old_report = sha256(f.report);
  using Writer = dna::BinaryStreamWriter;
  const std::uint16_t bad = 65534, one = 1, zero = 0;
  const float weight = 1.f;
  const std::vector<std::pair<std::string, std::function<void(Writer &)>>>
      cases = {
          {"joint-input",
           [&](Writer &w) { w.setJointGroupInputIndices(0, &bad, 1); }},
          {"joint-output",
           [&](Writer &w) { w.setJointGroupOutputIndices(0, &bad, 1); }},
          {"blend-input",
           [&](Writer &w) { w.setBlendShapeChannelInputIndices(&bad, 1); }},
          {"blend-output",
           [&](Writer &w) { w.setBlendShapeChannelOutputIndices(&bad, 1); }},
          {"blend-output-one",
           [&](Writer &w) { w.setBlendShapeChannelOutputIndices(&one, 1); }},
          {"gui-input", [&](Writer &w) { w.setGUIToRawInputIndices(&bad, 1); }},
          {"gui-output",
           [&](Writer &w) { w.setGUIToRawOutputIndices(&bad, 1); }},
          {"gui-lengths",
           [&](Writer &w) { w.setGUIToRawSlopeValues(nullptr, 0); }},
          {"blend-lod",
           [&](Writer &w) { w.setBlendShapeChannelLODs(&bad, 1); }},
          {"psd-output",
           [&](Writer &w) {
             w.setPSDCount(1);
             w.setPSDRowIndices(&zero, 1);
             w.setPSDColumnIndices(&zero, 1);
             w.setPSDValues(&weight, 1);
           }},
          {"psd-input", [&](Writer &w) {
             w.setPSDCount(1);
             w.setPSDRowIndices(&one, 1);
             w.setPSDColumnIndices(&bad, 1);
             w.setPSDValues(&weight, 1);
           }}};
  for (const auto &c : cases) {
    auto path = f.root.path / (c.first + ".dna");
    {
      auto input = rl4::makeScoped<rl4::FileStream>(
          f.options.input.u8string().c_str(), rl4::FileStream::AccessMode::Read,
          rl4::FileStream::OpenMode::Binary);
      auto reader = rl4::makeScoped<rl4::BinaryStreamReader>(input.get());
      reader->read();
      check_status("Read mutation source");
      auto output = rl4::makeScoped<rl4::FileStream>(
          path.u8string().c_str(), rl4::FileStream::AccessMode::Write,
          rl4::FileStream::OpenMode::Binary);
      auto writer = rl4::makeScoped<Writer>(output.get());
      writer->setFrom(reader.get());
      c.second(*writer);
      writer->write();
      check_status("Write mutation");
    }
    const auto result = arkit::test::execute(
        {binary.u8string(), path.u8string(), "--mapping",
         f.options.mapping.u8string(), "-o", f.options.output.u8string()},
        f.root.path);
    // A signal or sanitizer abort is a failure, even though it is nonzero.
    require(result.status == 1 &&
                result.output.find("Invalid DNA") != std::string::npos,
            c.first + " did not fail cleanly: " + result.output);
    require(sha256(f.options.output) == old_output &&
                sha256(f.report) == old_report,
            c.first + " changed existing outputs");
  }
}
void output_paths() {
  for (bool dna : {false, true}) {
    Fixture f;
    if (dna)
      f.options.output.replace_extension(".dna");
    f.previous_outputs();
    auto old_output = sha256(f.options.output);
    auto source_hash = sha256(f.options.input);
    auto mapping_hash = sha256(f.options.mapping);
    // A report that is the mapping itself must not be truncated.
    fs::remove(f.report);
    fs::copy_file(f.options.mapping, f.report);
    f.options.mapping = f.report;
    rejects([&] { bake(f.options, true); },
            "Mapping/report collision accepted");
    require(sha256(f.report) == mapping_hash &&
                sha256(f.options.output) == old_output,
            "Mapping/report collision destroyed a file");
    f.options.mapping = f.root.path / "mapping.json";
    fs::remove(f.report);
    fs::create_directory(f.report);
    text_file(f.report / "keep", "keep directory contents");
    rejects([&] { bake(f.options, true); }, "Report directory accepted");
    require(sha256(f.options.output) == old_output &&
                fs::exists(f.report / "keep"),
            "Report failure changed existing files");
    fs::remove_all(f.report);
    // Check both reports and exports against symlinks and source hard links.
    for (const auto &destination : {f.report, f.options.output}) {
      for (bool symlink : {false, true}) {
        if (fs::exists(destination))
          fs::remove(destination);
        std::error_code ec;
        if (symlink)
          fs::create_symlink(f.options.input, destination, ec);
        else
          fs::create_hard_link(f.options.input, destination, ec);
        if (ec) {
          std::cout << "Link creation unavailable: " << ec.message() << '\n';
          continue;
        }
        rejects([&] { bake(f.options, true); },
                "Input link accepted as destination");
        require(sha256(f.options.input) == source_hash &&
                    sha256(f.options.mapping) == mapping_hash,
                "Linked destination modified an input");
        if (destination == f.report)
          require(sha256(f.options.output) == old_output,
                  "Report link failure replaced export");
        fs::remove(destination);
      }
    }
    // Existing hard links to unrelated files are safely replaced as directory
    // entries; their other names must retain the original contents.
    auto unrelated = f.root.path / "unrelated";
    text_file(unrelated, "unrelated data");
    auto unrelated_hash = sha256(unrelated);
    std::error_code ec;
    fs::create_hard_link(unrelated, f.report, ec);
    if (!ec) {
      bake(f.options, true);
      require(sha256(unrelated) == unrelated_hash &&
                  read_json(f.report).contains("targets"),
              "Installing a report changed another hard-link name");
    }
  }
}
void rollback() {
  for (bool existing : {false, true}) {
    Fixture f;
    if (existing)
      text_file(f.options.output, "previous export");
    text_file(f.report, "previous report");
    const auto old_output = existing ? sha256(f.options.output) : std::string{};
    const auto old_report = sha256(f.report);
    TemporaryDirectory staging(f.root.path);
    text_file(staging.path / "export", "new export");
    // Fail the second install, after the first file has already been installed.
    rejects(
        [&] {
          install_outputs(staging,
                          {{staging.path / "export", f.options.output},
                           {staging.path / "missing-report", f.report}});
        },
        "Missing staged report accepted");
    require(sha256(f.report) == old_report, "Rollback did not restore report");
    require(existing ? sha256(f.options.output) == old_output
                     : !fs::exists(f.options.output),
            "Rollback did not restore export");
    require(!staging.path.empty(),
            "Successful rollback unnecessarily retained backups");
  }
  Fixture f;
  f.previous_outputs();
  // A successful replacement must update the matching pair and clean staging.
  bake(f.options, true);
  require(read_json(f.report).contains("source_sha256"),
          "Missing installed report");
  for (const auto &entry : fs::directory_iterator(f.root.path))
    require(entry.path().filename().u8string().find(".arkit-") != 0,
            "Successful bake leaked staging or backups");
}
#ifndef _WIN32
struct FileSizeLimit {
  rlimit previous{};
  struct sigaction previous_signal{};
  explicit FileSizeLimit(rlim_t bytes) {
    require(getrlimit(RLIMIT_FSIZE, &previous) == 0,
            "Cannot read file size limit");
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    require(sigaction(SIGXFSZ, &ignored, &previous_signal) == 0,
            "Cannot ignore file size signal");
    auto restricted = previous;
    restricted.rlim_cur = bytes;
    if (setrlimit(RLIMIT_FSIZE, &restricted) != 0) {
      sigaction(SIGXFSZ, &previous_signal, nullptr);
      throw std::runtime_error("Cannot set file size limit");
    }
  }
  ~FileSizeLimit() {
    setrlimit(RLIMIT_FSIZE, &previous);
    sigaction(SIGXFSZ, &previous_signal, nullptr);
  }
};
#endif
void write_errors() {
  Fixture f;
  f.previous_outputs();
  auto previous_report = sha256(f.report);
  // Serialization errors must be detected before the file is opened.
  rejects([&] { write_json(f.report, Json(std::string(1, char(0xff)))); },
          "Invalid report UTF-8 accepted");
  require(sha256(f.report) == previous_report,
          "Serialization error truncated report");
#ifndef _WIN32
  bake(f.options, true);
  auto size = fs::file_size(f.options.output);
  f.previous_outputs();
  auto old_output = sha256(f.options.output), old_report = sha256(f.report);
  {
    // Fail in the final buffered GLB write, which used to return success and
    // overwrite the previous export with a file one byte too short.
    FileSizeLimit limit(size - 1);
    rejects([&] { bake(f.options, true); }, "GLB close failure was ignored");
  }
  require(sha256(f.options.output) == old_output &&
              sha256(f.report) == old_report,
          "Short GLB write replaced existing outputs");
  auto mapping = read_json(f.options.mapping);
  mapping["padding"] = std::string(32768, 'x');
  write_json(f.options.mapping, mapping);
  bake(f.options, true);
  auto report_size = fs::file_size(f.report);
  f.previous_outputs();
  {
    // Allow the GLB to finish, then fail in the report's final buffered write.
    FileSizeLimit limit(report_size - 1);
    rejects([&] { bake(f.options, true); }, "Report close failure was ignored");
  }
  require(sha256(f.options.output) == old_output &&
              sha256(f.report) == old_report,
          "Short report write replaced existing outputs");
  {
    FileSizeLimit limit(63);
    rejects(
        [&] {
          write_json(f.root.path / "short.json", Json(std::string(128, 'x')));
        },
        "Small buffered report close failure was ignored");
  }
#endif
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 3, "Expected executable and test case");
    const std::string name = argv[2];
    if (name == "malformed-dna")
      malformed_dna(fs::absolute(fs::u8path(argv[1])));
    else if (name == "output-paths")
      output_paths();
    else if (name == "rollback")
      rollback();
    else if (name == "write-errors")
      write_errors();
    else
      throw std::runtime_error("Unknown safety test");
    std::cout << "PASS: " << name << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
