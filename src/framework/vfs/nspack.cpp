#include "framework/vfs/nspack.hpp"
#include "framework/vfs/vfs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ns {

// ---------------------------------------------------------------------------
// helpers (little-endian store/load; the format is LE on every host)
// ---------------------------------------------------------------------------
namespace {
void putU32(std::string& out, uint32_t v) {
  out.push_back((char)(v & 0xff));
  out.push_back((char)((v >> 8) & 0xff));
  out.push_back((char)((v >> 16) & 0xff));
  out.push_back((char)((v >> 24) & 0xff));
}
void putU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; i++) out.push_back((char)((v >> (8 * i)) & 0xff));
}
/** overwrite a u64 already appended at `at` (offset patching) */
void putU64At(std::string& out, uint64_t v, size_t at) {
  for (int i = 0; i < 8; i++) out[at + (size_t)i] = (char)((v >> (8 * i)) & 0xff);
}

uint32_t getU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

constexpr uint64_t kMaxFileCount = (1ull << 24);   // sanity cap
constexpr uint32_t kMaxNameLen = 4096;
constexpr uint64_t kMaxFileSize = (1ull << 40);    // 1 TiB cap

std::vector<uint8_t> readWholeFile(const std::string& path, uint64_t* sizeOut) {
  std::vector<uint8_t> out;
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return out;
  const std::streamoff end = f.tellg();
  if (end <= 0) return out;
  f.seekg(0, std::ios::beg);
  out.resize((size_t)end);
  if (!out.empty()) f.read((char*)out.data(), (std::streamsize)out.size());
  if (sizeOut) *sizeOut = (uint64_t)out.size();
  return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// FNV-1a 64
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const uint8_t* data, size_t n) {
  uint64_t h = 1469598103934665603ull;  // FNV offset basis
  for (size_t i = 0; i < n; i++) {
    h ^= data[i];
    h *= 1099511628211ull;  // FNV prime
  }
  return h;
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
bool PackageWriter::begin(const std::string& outPath, std::string* err) {
  outPath_ = outPath;
  files_.clear();
  totalBytes_ = 0;
  if (outPath_.empty()) {
    if (err) *err = "empty package output path";
    return false;
  }
  return true;
}

bool PackageWriter::addFile(const std::string& vpath, const std::vector<uint8_t>& data,
                            std::string* err) {
  const std::string n = normalizeVirtualPath(vpath);
  if (n.empty()) {
    if (err) *err = "refusing to package unsafe virtual path: " + vpath;
    return false;
  }
  if (n == kNspProductionMarker) {
    if (err) *err = "reserved virtual path: " + std::string(kNspProductionMarker);
    return false;
  }
  auto& e = files_[n];
  const uint64_t prev = e.data.size();
  e.name = n;
  e.data = data;
  e.hash = fnv1a64(data);
  totalBytes_ = totalBytes_ - prev + (uint64_t)e.data.size();
  return true;
}

bool PackageWriter::addFile(const std::string& vpath, const std::string& text,
                            std::string* err) {
  std::vector<uint8_t> bytes(text.begin(), text.end());
  return addFile(vpath, bytes, err);
}

bool PackageWriter::setProduction(const std::string& vpath, std::string* err) {
  const std::string n = normalizeVirtualPath(vpath);
  if (n.empty()) {
    if (err) *err = "unsafe production path: " + vpath;
    return false;
  }
  if (files_.count(n) == 0) {
    if (err) *err = "production path not added to the package: " + n;
    return false;
  }
  production_ = n;
  return true;
}

bool PackageWriter::finish(std::string* err) {
  if (outPath_.empty()) {
    if (err) *err = "PackageWriter::begin not called";
    return false;
  }
  std::ofstream f(outPath_, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (err) *err = "cannot create package: " + outPath_;
    return false;
  }

  // entries in deterministic (sorted) order; the .ns-production marker is a
  // synthetic entry whose content is the production's virtual path
  std::vector<const Entry*> entries;
  Entry marker;
  if (!production_.empty()) {
    marker.name = kNspProductionMarker;
    marker.data.assign(production_.begin(), production_.end());
    marker.hash = fnv1a64(marker.data);
  }
  entries.reserve(files_.size() + (marker.name.empty() ? 0 : 1));
  for (const auto& kv : files_) entries.push_back(&kv.second);
  if (!marker.name.empty()) entries.push_back(&marker);
  std::sort(entries.begin(), entries.end(),
            [](const Entry* a, const Entry* b) { return a->name < b->name; });

  // manifest table
  std::string manifest;
  std::vector<uint64_t> dataOffsets(entries.size(), 0);
  uint64_t cursor = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = *entries[i];
    const uint32_t nl = (uint32_t)e.name.size();
    const size_t entryStart = manifest.size();
    putU32(manifest, nl);
    manifest.append(e.name);
    // pad the entry to an 8-byte boundary
    while ((manifest.size() - entryStart) % 8 != 0) manifest.push_back('\0');
    putU64(manifest, 0);  // offset - patched below
    putU64(manifest, (uint64_t)e.data.size());  // packedSize == size (method 0)
    putU64(manifest, (uint64_t)e.data.size());  // uncompressed size
    putU32(manifest, kNspMethodStore);
    putU32(manifest, 0);  // flags
    putU64(manifest, e.hash);
    dataOffsets[i] = cursor;
    cursor += (uint64_t)e.data.size();
  }

  const uint64_t manifestOffset = kNspHeaderSize;
  const uint64_t manifestSize = (uint64_t)manifest.size();
  // data starts right after the manifest, 8-aligned
  uint64_t dataOffset = manifestOffset + manifestSize;
  dataOffset = (dataOffset + 7) & ~7ull;

  // patch entry offsets (offset = dataOffset + running cursor). The offset
  // field sits at 4 + nameLen, padded to an 8-byte boundary, within each
  // entry; manifestPos tracks the entry's absolute start in the manifest.
  uint64_t manifestPos = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    const size_t nameEnd = 4 + (size_t)entries[i]->name.size();
    const size_t pad = (8 - (nameEnd % 8)) % 8;
    putU64At(manifest, dataOffset + dataOffsets[i], manifestPos + nameEnd + pad);
    manifestPos += nameEnd + pad + 40;
  }

  // header
  std::string header;
  header.append(kNspMagic, 4);
  putU32(header, kNspVersion);
  putU32(header, 0);  // flags
  putU32(header, (uint32_t)entries.size());
  putU64(header, manifestOffset);
  putU64(header, manifestSize);
  putU64(header, dataOffset);
  putU64(header, 0);  // reserved
  while (header.size() < kNspHeaderSize) header.push_back('\0');

  f.write(header.data(), (std::streamsize)header.size());
  f.write(manifest.data(), (std::streamsize)manifest.size());
  const uint64_t pad = dataOffset - (manifestOffset + manifestSize);
  for (uint64_t i = 0; i < pad; i++) f.put('\0');
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = *entries[i];
    if (!e.data.empty())
      f.write((const char*)e.data.data(), (std::streamsize)e.data.size());
  }
  f.flush();
  if (!f) {
    if (err) *err = "write failed while creating package: " + outPath_;
    f.close();
    std::error_code ec;
    std::filesystem::remove(outPath_, ec);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------
bool PackageReader::open(const std::string& path, std::string* err) {
  entries_.clear();
  path_.clear();
  lastError_.clear();
  uint64_t fileSize = 0;
  const std::vector<uint8_t> raw = readWholeFile(path, &fileSize);
  if (raw.empty()) {
    if (err) *err = "cannot open package (missing or empty): " + path;
    return false;
  }
  if (fileSize < kNspHeaderSize) {
    if (err) *err = "package too small for header: " + path;
    return false;
  }
  const uint8_t* h = raw.data();
  if (std::memcmp(h, kNspMagic, 4) != 0) {
    if (err) *err = "not a Null Sector package (bad magic): " + path;
    return false;
  }
  const uint32_t version = getU32(h + 4);
  if (version != kNspVersion) {
    if (err) *err = "unsupported package version " + std::to_string(version) +
                    " (reader supports " + std::to_string(kNspVersion) + "): " + path;
    return false;
  }
  const uint32_t fileCount = getU32(h + 12);
  if (fileCount > kMaxFileCount) {
    if (err) *err = "package file count out of range: " + path;
    return false;
  }
  const uint64_t manifestOffset = getU64(h + 16);
  const uint64_t manifestSize = getU64(h + 24);
  const uint64_t dataOffset = getU64(h + 32);

  if (manifestOffset != kNspHeaderSize) {
    if (err) *err = "package manifest offset invalid: " + path;
    return false;
  }
  if (manifestOffset + manifestSize > fileSize) {
    if (err) *err = "package manifest exceeds file size (truncated?): " + path;
    return false;
  }
  if (dataOffset < manifestOffset + manifestSize || dataOffset > fileSize) {
    if (err) *err = "package data offset invalid (truncated?): " + path;
    return false;
  }

  // parse the manifest
  uint64_t pos = manifestOffset;
  std::map<std::string, Entry> parsed;
  for (uint32_t i = 0; i < fileCount; i++) {
    if (pos + 4 > manifestOffset + manifestSize) {
      if (err) *err = "package manifest truncated: " + path;
      return false;
    }
    const uint32_t nameLen = getU32(raw.data() + pos);
    pos += 4;
    if (nameLen == 0 || nameLen > kMaxNameLen) {
      if (err) *err = "package entry name length invalid: " + path;
      return false;
    }
    if (pos + nameLen > manifestOffset + manifestSize) {
      if (err) *err = "package entry name truncated: " + path;
      return false;
    }
    const std::string name((const char*)raw.data() + pos, nameLen);
    pos += nameLen;
    if (normalizeVirtualPath(name) != name) {
      if (err) *err = "package entry has an unsafe virtual path: '" + name + "'";
      return false;
    }
    pos = (pos + 7) & ~7ull;
    if (pos + 40 > manifestOffset + manifestSize) {
      if (err) *err = "package manifest entry truncated: " + path;
      return false;
    }
    const uint8_t* e = raw.data() + pos;
    Entry en;
    en.name = name;
    en.offset = getU64(e);
    en.packedSize = getU64(e + 8);
    en.size = getU64(e + 16);
    en.method = getU32(e + 24);
    en.hash = getU64(e + 32);
    pos += 40;

    if (en.method != kNspMethodStore) {
      if (err) *err = "package uses unsupported compression method " +
                      std::to_string(en.method) + " for '" + name + "'";
      return false;
    }
    if (en.packedSize != en.size) {
      if (err) *err = "package entry packed/uncompressed size mismatch (method 0): '" +
                      name + "'";
      return false;
    }
    if (en.size > kMaxFileSize) {
      if (err) *err = "package entry too large: '" + name + "'";
      return false;
    }
    if (en.offset < dataOffset || en.offset + en.packedSize > fileSize) {
      if (err) *err = "package entry offset out of range (truncated?): '" + name + "'";
      return false;
    }
    if (!parsed.emplace(name, std::move(en)).second) {
      if (err) *err = "duplicate package entry: '" + name + "'";
      return false;
    }
  }
  if (pos != manifestOffset + manifestSize) {
    if (err) *err = "package manifest size mismatch (trailing data): " + path;
    return false;
  }

  raw_ = std::move(raw);
  entries_ = std::move(parsed);
  path_ = path;
  return true;
}

bool PackageReader::has(const std::string& vpath) const {
  const std::string n = normalizeVirtualPath(vpath);
  return !n.empty() && entries_.count(n) != 0;
}

uint64_t PackageReader::fileSize(const std::string& vpath) const {
  const std::string n = normalizeVirtualPath(vpath);
  const auto it = entries_.find(n);
  return it == entries_.end() ? 0 : it->second.size;
}

std::vector<uint8_t> PackageReader::read(const std::string& vpath) const {
  lastError_.clear();
  const std::string n = normalizeVirtualPath(vpath);
  if (n.empty()) {
    lastError_ = "unsafe virtual path: " + vpath;
    return {};
  }
  const auto it = entries_.find(n);
  if (it == entries_.end()) {
    lastError_ = "not in package: " + n;
    return {};
  }
  const Entry& e = it->second;
  if (e.offset + e.packedSize > raw_.size() || e.packedSize != e.size) {
    lastError_ = "package entry out of range (truncated?): " + n;
    return {};
  }
  std::vector<uint8_t> out(raw_.begin() + (ptrdiff_t)e.offset,
                           raw_.begin() + (ptrdiff_t)(e.offset + e.size));
  if (fnv1a64(out) != e.hash) {
    lastError_ = "checksum mismatch (corrupt data): " + n;
    out.clear();
    return out;
  }
  return out;
}

std::string PackageReader::readText(const std::string& vpath) const {
  const std::vector<uint8_t> bytes = read(vpath);
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::string> PackageReader::fileList() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& kv : entries_) out.push_back(kv.first);
  return out;
}

bool PackageReader::verifyAll(std::string* err) const {
  for (const auto& kv : entries_) {
    const Entry& e = kv.second;
    if (e.offset + e.packedSize > raw_.size()) {
      if (err) *err = "package entry out of range: " + e.name;
      return false;
    }
    const uint64_t h = fnv1a64(raw_.data() + e.offset, (size_t)e.size);
    if (h != e.hash) {
      if (err) *err = "checksum mismatch: " + e.name;
      return false;
    }
  }
  return true;
}

}  // namespace ns
