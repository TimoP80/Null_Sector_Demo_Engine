// only the deflate/inflate core is needed; archive APIs compile out
#define MINIZ_NO_ARCHIVE_APIS
#include "framework/vfs/miniz/miniz.h"
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

/** DEFLATE (zlib stream) via miniz. miniz's mz_ulong is 32-bit on Windows,
 *  so refuse inputs that would not fit - the practical asset ceiling here
 *  is far below 4 GiB anyway. Returns false when the input is empty or the
 *  stream could not be produced. */
bool deflateBytes(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  if (in.empty() || in.size() > 0xFFFFFFFFull) return false;
  mz_ulong bound = mz_compressBound((mz_ulong)in.size());
  out.resize(bound);
  mz_ulong outLen = bound;
  const int rc =
      mz_compress2(out.data(), &outLen, in.data(), (mz_ulong)in.size(),
                   MZ_DEFAULT_COMPRESSION);
  if (rc != MZ_OK) return false;
  out.resize(outLen);
  return true;
}

/** Inflate a zlib stream; the decompressed size must match expectedOut. */
bool inflateBytes(const uint8_t* in, size_t inLen, size_t expectedOut,
                  std::vector<uint8_t>& out) {
  if (expectedOut == 0) {
    out.clear();
    return true;
  }
  if (inLen > 0xFFFFFFFFull || expectedOut > 0xFFFFFFFFull) return false;
  out.resize(expectedOut);
  mz_ulong outLen = (mz_ulong)expectedOut;
  const int rc = mz_uncompress(out.data(), &outLen, in, (mz_ulong)inLen);
  if (rc != MZ_OK || outLen != expectedOut) {
    out.clear();
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// FNV-1a 64
// ---------------------------------------------------------------------------
uint64_t fnv1a64(const uint8_t* data, size_t n) {
  uint64_t h = 14695981039346656037ull;  // FNV-1a 64 offset basis (0xcbf29ce484222325)
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
                            std::string* err, bool compress) {
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
  e.compressHint = compress;
  totalBytes_ = totalBytes_ - prev + (uint64_t)e.data.size();
  return true;
}

bool PackageWriter::addFile(const std::string& vpath, const std::string& text,
                            std::string* err, bool compress) {
  std::vector<uint8_t> bytes(text.begin(), text.end());
  return addFile(vpath, bytes, err, compress);
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

  // decide the stored form of every entry up front: DEFLATE when the hint is
  // set AND the compressed stream is genuinely smaller, otherwise store. The
  // hash always covers the UNCOMPRESSED bytes, so integrity means the same
  // thing for both methods.
  stats_ = WriteStats();
  std::vector<std::vector<uint8_t>> payloads(entries.size());
  std::vector<uint32_t> methods(entries.size(), kNspMethodStore);
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = *entries[i];
    std::vector<uint8_t> compressed;
    uint32_t method = kNspMethodStore;
    const std::vector<uint8_t>* payload = &e.data;
    if (e.compressHint && deflateBytes(e.data, compressed) &&
        compressed.size() < e.data.size()) {
      method = kNspMethodDeflate;
      payload = &compressed;
    }
    payloads[i] = *payload;
    methods[i] = method;
    MethodStats& ms = method == kNspMethodDeflate ? stats_.deflate : stats_.store;
    ms.count++;
    ms.rawBytes += (uint64_t)e.data.size();
    ms.storedBytes += (uint64_t)payload->size();
  }

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
    putU64(manifest, (uint64_t)payloads[i].size());  // bytes stored in package
    putU64(manifest, (uint64_t)e.data.size());       // uncompressed size
    putU32(manifest, methods[i]);
    putU32(manifest, 0);  // flags
    putU64(manifest, e.hash);
    dataOffsets[i] = cursor;
    cursor += (uint64_t)payloads[i].size();
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
    const std::vector<uint8_t>& payload = payloads[i];
    if (!payload.empty())
      f.write((const char*)payload.data(), (std::streamsize)payload.size());
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
// open() validates the header + manifest and keeps ONLY those in RAM (plus
// the per-entry index). Payloads are read on demand: read() opens the file
// and seeks to the entry's offset, so a multi-GB package costs a few KB of
// memory, not the whole archive. All bounds arithmetic is overflow-safe
// (offset + size can never wrap past the file), and entries must not
// overlap.
// ---------------------------------------------------------------------------
bool PackageReader::open(const std::string& path, std::string* err) {
  entries_.clear();
  dirs_.clear();
  path_.clear();
  lastError_.clear();
  fileSize_ = 0;

  std::error_code ec;
  const uint64_t fsize = std::filesystem::file_size(path, ec);
  if (ec || fsize < kNspHeaderSize) {
    if (err) *err = "package too small for header: " + path;
    return false;
  }
  fileSize_ = fsize;

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open package: " + path;
    return false;
  }
  uint8_t header[kNspHeaderSize];
  f.read((char*)header, (std::streamsize)kNspHeaderSize);
  if (f.gcount() != (std::streamsize)kNspHeaderSize) {
    if (err) *err = "package too small for header: " + path;
    return false;
  }
  const uint8_t* h = header;
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
  const uint32_t headerFlags = getU32(h + 8);
  if (headerFlags != 0) {
    if (err) *err = "package header flags nonzero (unsupported for v1): " + path;
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
  const uint64_t reserved = getU64(h + 40);
  if (reserved != 0) {
    if (err) *err = "package reserved field nonzero: " + path;
    return false;
  }

  if (manifestOffset != kNspHeaderSize) {
    if (err) *err = "package manifest offset invalid: " + path;
    return false;
  }
  // overflow-safe: manifestOffset + manifestSize must not wrap and must fit
  if (manifestOffset > fileSize_ || manifestSize > fileSize_ - manifestOffset) {
    if (err) *err = "package manifest exceeds file size (truncated?): " + path;
    return false;
  }
  const uint64_t manifestEnd = manifestOffset + manifestSize;
  if (dataOffset < manifestEnd || dataOffset > fileSize_) {
    if (err) *err = "package data offset invalid (truncated?): " + path;
    return false;
  }
  if ((dataOffset & 7) != 0) {
    if (err) *err = "package data offset not 8-aligned: " + path;
    return false;
  }

  // read only the manifest region (header + manifest stay in RAM)
  std::vector<uint8_t> manifest((size_t)manifestSize);
  f.seekg((std::streamoff)manifestOffset, std::ios::beg);
  f.read((char*)manifest.data(), (std::streamsize)manifestSize);
  if (f.gcount() != (std::streamsize)manifestSize) {
    if (err) *err = "package manifest unreadable (truncated?): " + path;
    return false;
  }

  // parse the manifest
  uint64_t pos = manifestOffset;
  std::map<std::string, Entry> parsed;
  for (uint32_t i = 0; i < fileCount; i++) {
    // overflow-safe in-manifest bounds
    if (pos >= manifestEnd || 4 > manifestEnd - pos) {
      if (err) *err = "package manifest truncated: " + path;
      return false;
    }
    const uint32_t nameLen = getU32(manifest.data() + (pos - manifestOffset));
    pos += 4;
    if (nameLen == 0 || nameLen > kMaxNameLen) {
      if (err) *err = "package entry name length invalid: " + path;
      return false;
    }
    if (pos >= manifestEnd || nameLen > manifestEnd - pos) {
      if (err) *err = "package entry name truncated: " + path;
      return false;
    }
    const std::string name((const char*)manifest.data() + (pos - manifestOffset),
                           nameLen);
    pos += nameLen;
    if (normalizeVirtualPath(name) != name) {
      if (err) *err = "package entry has an unsafe virtual path: '" + name + "'";
      return false;
    }
    pos = (pos + 7) & ~7ull;
    if (pos >= manifestEnd || 40 > manifestEnd - pos) {
      if (err) *err = "package manifest entry truncated: " + path;
      return false;
    }
    const uint8_t* e = manifest.data() + (pos - manifestOffset);
    Entry en;
    en.name = name;
    en.offset = getU64(e);
    en.packedSize = getU64(e + 8);
    en.size = getU64(e + 16);
    en.method = getU32(e + 24);
    const uint32_t entryFlags = getU32(e + 28);
    en.hash = getU64(e + 32);
    pos += 40;

    if (entryFlags != 0) {
      if (err) *err = "package entry flags nonzero (unsupported for v1): '" +
                      name + "'";
      return false;
    }
    if (en.method != kNspMethodStore && en.method != kNspMethodDeflate) {
      if (err) *err = "package uses unsupported compression method " +
                      std::to_string(en.method) + " for '" + name + "'";
      return false;
    }
    if (en.method == kNspMethodStore && en.packedSize != en.size) {
      if (err) *err = "package entry packed/uncompressed size mismatch (method 0): '" +
                      name + "'";
      return false;
    }
    if (en.method == kNspMethodDeflate && en.size == 0) {
      if (err) *err = "package entry deflate with empty payload: '" + name + "'";
      return false;
    }
    if (en.size > kMaxFileSize) {
      if (err) *err = "package entry too large: '" + name + "'";
      return false;
    }
    // overflow-safe: en.offset + en.packedSize must fit inside the file
    if (en.offset < dataOffset || en.offset > fileSize_ ||
        en.packedSize > fileSize_ - en.offset) {
      if (err) *err = "package entry offset out of range (truncated?): '" + name + "'";
      return false;
    }
    if (!parsed.emplace(name, std::move(en)).second) {
      if (err) *err = "duplicate package entry: '" + name + "'";
      return false;
    }
  }
  if (pos != manifestEnd) {
    if (err) *err = "package manifest size mismatch (trailing data): " + path;
    return false;
  }

  // reject overlapping payload ranges (defense in depth - the writer never
  // emits them, so any overlap means a hand-crafted or corrupted package)
  {
    std::vector<const Entry*> byOff;
    byOff.reserve(parsed.size());
    for (const auto& kv : parsed) byOff.push_back(&kv.second);
    std::sort(byOff.begin(), byOff.end(),
              [](const Entry* a, const Entry* b) { return a->offset < b->offset; });
    for (size_t i = 1; i < byOff.size(); i++) {
      const Entry* prev = byOff[i - 1];
      const Entry* cur = byOff[i];
      if (prev->offset + prev->packedSize > cur->offset) {
        if (err)
          *err = "package entries overlap: '" + prev->name + "' and '" + cur->name + "'";
        return false;
      }
    }
  }

  // directory index: parent vdir -> direct children (full virtual paths).
  // Every path segment of every file becomes a child of its parent, so
  // list("") yields top-level dirs + bare files, and list("data/sub")
  // yields "data/sub/deep.txt" - O(children) instead of an O(all) scan.
  for (const auto& kv : parsed) {
    const std::string& p = kv.first;
    size_t start = 0;
    std::string parent;
    while (true) {
      const size_t slash = p.find('/', start);
      if (slash == std::string::npos) {
        dirs_[parent].insert(p);
        break;
      }
      const std::string child = p.substr(0, slash);
      dirs_[parent].insert(child);
      parent = child;
      start = slash + 1;
    }
  }

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

uint32_t PackageReader::method(const std::string& vpath) const {
  const std::string n = normalizeVirtualPath(vpath);
  const auto it = entries_.find(n);
  return it == entries_.end() ? kNspMethodStore : it->second.method;
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
  // overflow-safe bounds (open() validated these, but the file may have
  // changed on disk since - re-validate against the size captured at open)
  if (e.offset > fileSize_ || e.packedSize > fileSize_ - e.offset) {
    lastError_ = "package entry out of range (truncated?): " + n;
    return {};
  }
  std::ifstream f(path_, std::ios::binary);
  if (!f) {
    lastError_ = "cannot reopen package: " + path_;
    return {};
  }
  std::vector<uint8_t> stored((size_t)e.packedSize);
  if (!stored.empty()) {
    f.seekg((std::streamoff)e.offset, std::ios::beg);
    f.read((char*)stored.data(), (std::streamsize)stored.size());
  }
  if (f.gcount() != (std::streamsize)stored.size()) {
    lastError_ = "package entry short read (truncated?): " + n;
    return {};
  }
  std::vector<uint8_t> out;
  if (e.method == kNspMethodDeflate) {
    if (!inflateBytes(stored.data(), stored.size(), (size_t)e.size, out)) {
      lastError_ = "corrupt compressed data: " + n;
      return {};
    }
  } else {
    out = std::move(stored);
  }
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

bool PackageReader::isDirectory(const std::string& vdir) const {
  const std::string n = normalizeVirtualPath(vdir);
  return !n.empty() && dirs_.count(n) != 0;
}

std::vector<std::string> PackageReader::listDirectory(const std::string& vdir) const {
  std::vector<std::string> out;
  const std::string n = normalizeVirtualPath(vdir);
  const auto it = dirs_.find(n);
  if (it == dirs_.end()) return out;
  out.reserve(it->second.size());
  for (const auto& child : it->second) out.push_back(child);
  return out;
}

bool PackageReader::verifyAll(std::string* err) const {
  std::ifstream f(path_, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot reopen package: " + path_;
    return false;
  }
  for (const auto& kv : entries_) {
    const Entry& e = kv.second;
    if (e.offset > fileSize_ || e.packedSize > fileSize_ - e.offset) {
      if (err) *err = "package entry out of range: " + e.name;
      return false;
    }
    std::vector<uint8_t> stored((size_t)e.packedSize);
    if (!stored.empty()) {
      f.seekg((std::streamoff)e.offset, std::ios::beg);
      f.read((char*)stored.data(), (std::streamsize)stored.size());
      if (f.gcount() != (std::streamsize)stored.size()) {
        if (err) *err = "package entry short read: " + e.name;
        return false;
      }
    }
    std::vector<uint8_t> buf;
    if (e.method == kNspMethodDeflate) {
      if (!inflateBytes(stored.data(), stored.size(), (size_t)e.size, buf)) {
        if (err) *err = "package entry corrupt compressed data: " + e.name;
        return false;
      }
    } else {
      buf = std::move(stored);
    }
    const uint64_t h = fnv1a64(buf);
    if (h != e.hash) {
      if (err) *err = "checksum mismatch: " + e.name;
      return false;
    }
  }
  return true;
}

}  // namespace ns
