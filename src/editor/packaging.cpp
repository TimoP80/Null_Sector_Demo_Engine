// ---------------------------------------------------------------------------
// Editor production packaging implementation.
// ---------------------------------------------------------------------------
#include "editor/packaging.hpp"

#include "app/packer.hpp"
#include "framework/vfs/nspack.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace ns {
namespace {

namespace fs = std::filesystem;

std::string safeStem(const std::string& raw) {
  std::string out;
  for (const unsigned char c : raw) {
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.') out += (char)c;
    else out += '_';
  }
  while (!out.empty() && out.back() == '.') out.pop_back();
  return out.empty() ? "project" : out;
}

bool relativeUnder(const fs::path& root, const fs::path& file, fs::path& rel) {
  std::error_code ec;
  const fs::path r = fs::weakly_canonical(root, ec);
  if (ec) return false;
  const fs::path f = fs::weakly_canonical(file, ec);
  if (ec) return false;
  rel = f.lexically_relative(r);
  if (rel.empty() || rel == "." || rel == "..") return false;
  const auto first = rel.begin();
  return first == rel.end() || *first != "..";
}

void put16(std::ofstream& out, uint16_t v) {
  const char b[2] = {(char)(v & 0xff), (char)((v >> 8) & 0xff)};
  out.write(b, sizeof b);
}

void put32(std::ofstream& out, uint32_t v) {
  const char b[4] = {(char)(v & 0xff), (char)((v >> 8) & 0xff),
                     (char)((v >> 16) & 0xff), (char)((v >> 24) & 0xff)};
  out.write(b, sizeof b);
}

uint32_t crc32(const std::vector<uint8_t>& data) {
  uint32_t crc = 0xffffffffu;
  for (const uint8_t byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i)
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1));
  }
  return ~crc;
}

struct ZipEntry {
  std::string name;
  uint32_t crc = 0;
  uint32_t size = 0;
  uint32_t localOffset = 0;
};

bool readFile(const fs::path& path, std::vector<uint8_t>& data,
              std::string& error) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    error = "cannot read " + path.string();
    return false;
  }
  const std::streamoff end = in.tellg();
  if (end < 0 || (uint64_t)end > std::numeric_limits<uint32_t>::max()) {
    error = "file is too large for the distribution ZIP: " + path.string();
    return false;
  }
  data.resize((size_t)end);
  in.seekg(0, std::ios::beg);
  if (!data.empty()) in.read(reinterpret_cast<char*>(data.data()), end);
  if (!in) {
    error = "cannot read complete file " + path.string();
    return false;
  }
  return true;
}

/** Write a conventional ZIP32 archive with stored entries. The NSP already
 *  compresses text/shader assets, and storing the three distribution files
 *  avoids an external ZIP dependency while remaining universally readable. */
bool writeZip(const fs::path& sourceDir, const fs::path& output,
              std::string& error) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(
           sourceDir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      error = "cannot enumerate package staging directory: " + ec.message();
      return false;
    }
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot create distribution ZIP: " + output.string();
    return false;
  }

  std::vector<ZipEntry> entries;
  uint64_t position = 0;
  for (const fs::path& file : files) {
    const std::string name = file.lexically_relative(sourceDir).generic_string();
    if (name.empty() || name.size() > std::numeric_limits<uint16_t>::max()) {
      error = "ZIP entry name is too long: " + name;
      out.close();
      fs::remove(output, ec);
      return false;
    }
    std::vector<uint8_t> data;
    if (!readFile(file, data, error)) {
      out.close();
      fs::remove(output, ec);
      return false;
    }
    const uint64_t localSize = 30ull + name.size() + data.size();
    if (position > std::numeric_limits<uint32_t>::max() ||
        localSize > std::numeric_limits<uint32_t>::max() - position) {
      error = "distribution is too large for ZIP32";
      out.close();
      fs::remove(output, ec);
      return false;
    }

    ZipEntry entry;
    entry.name = name;
    entry.crc = crc32(data);
    entry.size = (uint32_t)data.size();
    entry.localOffset = (uint32_t)position;

    put32(out, 0x04034b50u);  // local file header
    put16(out, 20);            // version needed
    put16(out, 0);             // flags: UTF-8 is not needed for generated names
    put16(out, 0);             // method: stored
    put16(out, 0);             // DOS time
    put16(out, 0);             // DOS date
    put32(out, entry.crc);
    put32(out, entry.size);
    put32(out, entry.size);
    put16(out, (uint16_t)name.size());
    put16(out, 0);             // extra length
    out.write(name.data(), (std::streamsize)name.size());
    if (!data.empty())
      out.write(reinterpret_cast<const char*>(data.data()),
                (std::streamsize)data.size());
    if (!out) {
      error = "cannot write distribution ZIP";
      out.close();
      fs::remove(output, ec);
      return false;
    }
    entries.push_back(std::move(entry));
    position += localSize;
  }

  const uint64_t centralOffset = position;
  for (const ZipEntry& entry : entries) {
    put32(out, 0x02014b50u);  // central directory header
    put16(out, 20);            // made by
    put16(out, 20);            // version needed
    put16(out, 0);             // flags
    put16(out, 0);             // method: stored
    put16(out, 0);             // DOS time
    put16(out, 0);             // DOS date
    put32(out, entry.crc);
    put32(out, entry.size);
    put32(out, entry.size);
    put16(out, (uint16_t)entry.name.size());
    put16(out, 0);             // extra length
    put16(out, 0);             // comment length
    put16(out, 0);             // disk number
    put16(out, 0);             // internal attributes
    put32(out, 0);              // external attributes
    put32(out, entry.localOffset);
    out.write(entry.name.data(), (std::streamsize)entry.name.size());
    position += 46ull + entry.name.size();
  }
  const uint64_t centralSize = position - centralOffset;
  if (entries.size() > std::numeric_limits<uint16_t>::max() ||
      centralOffset > std::numeric_limits<uint32_t>::max() ||
      centralSize > std::numeric_limits<uint32_t>::max()) {
    error = "distribution is too large for ZIP32 central directory";
    out.close();
    fs::remove(output, ec);
    return false;
  }

  put32(out, 0x06054b50u);  // end of central directory
  put16(out, 0);
  put16(out, 0);
  put16(out, (uint16_t)entries.size());
  put16(out, (uint16_t)entries.size());
  put32(out, (uint32_t)centralSize);
  put32(out, (uint32_t)centralOffset);
  put16(out, 0);
  out.flush();
  const bool ok = (bool)out;
  out.close();
  if (!ok) {
    error = "cannot finalize distribution ZIP";
    fs::remove(output, ec);
    return false;
  }
  return true;
}

}  // namespace

EditorPackageResult packageEditorProject(const std::string& projectRoot,
                                         const std::string& scriptPath,
                                         const std::string& trackPath,
                                         const std::string& executablePath,
                                         const std::string& outputZip) {
  EditorPackageResult result;
  result.outputZip = outputZip;

  std::error_code ec;
  const fs::path root = fs::weakly_canonical(fs::path(projectRoot), ec);
  if (ec || !fs::is_directory(root, ec)) {
    result.message = "project root does not exist: " + projectRoot;
    return result;
  }
  const fs::path script = fs::weakly_canonical(fs::path(scriptPath), ec);
  if (ec || !fs::is_regular_file(script, ec)) {
    result.message = "project script does not exist: " + scriptPath;
    return result;
  }

  fs::path scriptRel;
  if (!relativeUnder(root, script, scriptRel) || scriptRel.extension() != ".nsd") {
    result.message = "the .nsd must be inside the engine project tree: " + scriptPath;
    return result;
  }

  const fs::path executable = fs::path(executablePath);
  if (executable.empty() || !fs::is_regular_file(executable, ec)) {
    result.message = "engine executable not found: " + executablePath;
    return result;
  }

  const fs::path zipPath = fs::absolute(fs::path(outputZip), ec);
  if (ec) {
    result.message = "invalid distribution output path: " + outputZip;
    return result;
  }
  fs::create_directories(zipPath.parent_path(), ec);
  if (ec) {
    result.message = "cannot create output directory: " +
                     zipPath.parent_path().string();
    return result;
  }

  const std::string stem = safeStem(script.stem().string());
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const fs::path stage = zipPath.parent_path() /
                         ("." + stem + ".package-" + std::to_string(stamp));
  fs::create_directories(stage, ec);
  if (ec) {
    result.message = "cannot create package staging directory: " + stage.string();
    return result;
  }
  const auto cleanup = [&] {
    std::error_code cleanupEc;
    fs::remove_all(stage, cleanupEc);
  };
  const auto fail = [&](const std::string& why) {
    cleanup();
    result.message = why;
    return result;
  };

  const fs::path nsp = stage / (stem + ".nsp");
  std::string trackArg;
  if (!trackPath.empty()) {
    const fs::path track = fs::weakly_canonical(fs::path(trackPath), ec);
    if (ec || !fs::is_regular_file(track, ec))
      return fail("selected audio track does not exist: " + trackPath);
    fs::path trackRel;
    if (!relativeUnder(root, track, trackRel))
      return fail("selected audio track is outside the project tree: " + trackPath);
    trackArg = trackRel.generic_string();
  }

  // The production packer expects paths relative to a tree containing data/,
  // shaders/ and assets/. It follows the same references as --pack, so the
  // editor and CLI cannot produce different NSP contents.
  const int packRc = runProductionPacker(root.string(), scriptRel.generic_string(),
                                         trackArg, nsp.string());
  if (packRc != 0 || !fs::is_regular_file(nsp, ec))
    return fail("production pack failed; see the packer output in the Console");

  PackageReader reader;
  std::string verifyError;
  if (!reader.open(nsp.string(), &verifyError) ||
      !reader.verifyAll(&verifyError)) {
    return fail("created NSP failed verification: " + verifyError);
  }
  result.assetCount = reader.fileCount();
  result.nspBytes = fs::file_size(nsp, ec);

  const fs::path shippedExe = stage / (stem + ".exe");
  fs::copy_file(executable, shippedExe, fs::copy_options::overwrite_existing, ec);
  if (ec) return fail("cannot copy engine executable: " + ec.message());

  const fs::path launch = stage / "launch.bat";
  {
    std::ofstream bat(launch, std::ios::binary);
    if (!bat) return fail("cannot create launch.bat");
    bat << "@echo off\r\n"
        << "setlocal\r\n"
        << "cd /d \"%~dp0\"\r\n"
        << "\"%~dp0" << stem << ".exe\""
        << " --play=\"%~dp0" << stem << ".nsp\" --fullscreen\r\n"
        << "if errorlevel 1 (\r\n"
        << "  echo The demo exited with an error.\r\n"
        << "  pause\r\n"
        << ")\r\n";
  }

  std::string zipError;
  if (!writeZip(stage, zipPath, zipError)) return fail(zipError);
  result.zipBytes = fs::file_size(zipPath, ec);
  cleanup();
  result.outputZip = zipPath.string();
  result.ok = true;
  result.message = "created " + stem + ".nsp, " + stem +
                   ".exe and launch.bat (" + std::to_string(result.assetCount) +
                   " assets)";
  return result;
}

}  // namespace ns
