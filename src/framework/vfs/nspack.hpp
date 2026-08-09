// ---------------------------------------------------------------------------
// NSPACK - the Null Sector package (.nsp) format, version 1.
//
// A .nsp is a self-contained, read-only archive of virtual asset paths
// (data/..., shaders/..., assets/...) with per-file integrity hashes. The
// format is deliberately simple and stable so future versions can extend it
// without breaking old packages.
//
//   Layout (all integers little-endian):
//
//   Header (48 bytes):
//     u8[4]  magic            "NSPK"
//     u32    version          1
//     u32    flags            0
//     u32    fileCount
//     u64    manifestOffset   absolute offset of the manifest table
//     u64    manifestSize     total bytes of the manifest table
//     u64    dataOffset       absolute offset of the first file payload
//                             (== manifestOffset + manifestSize, 8-aligned)
//     u64    reserved         0
//
//   Manifest (fileCount entries, each 8-aligned):
//     u32    nameLen
//     u8[nameLen] name        UTF-8 virtual path ('/' separators)
//     u8[pad]                 zero padding to an 8-byte boundary
//     u64    offset           absolute file offset of the payload
//     u64    packedSize       bytes stored in the package
//     u64    size             uncompressed size
//     u32    method           0 = store (packedSize == size),
//                            1 = DEFLATE (zlib stream; size > packedSize)
//     u32    flags            0
//     u64    hash             FNV-1a 64 of the UNCOMPRESSED bytes
//
//   Data:     fileCount payloads, each at offset, 8-aligned start.
//
// Validation is defensive: a truncated file, out-of-range offset/size, bad
// magic, unsupported version, unsafe virtual path, or checksum mismatch is
// detected and reported instead of crashing. Method 1 is DEFLATE (a zlib
// stream, produced/consumed via the vendored miniz in miniz/): the payload
// is compressed but the hash still covers the UNCOMPRESSED bytes, so an
// asset's integrity check means the same thing regardless of method.
//
//   PackageWriter  builds a .nsp (buffers the files, writes on finish()).
//   PackageReader  opens + validates a .nsp and serves random-access reads.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ns {

constexpr char kNspMagic[4] = {'N', 'S', 'P', 'K'};
constexpr uint32_t kNspVersion = 1;
constexpr uint64_t kNspHeaderSize = 48;
constexpr uint32_t kNspMethodStore = 0;   // stored as-is (packedSize == size)
constexpr uint32_t kNspMethodDeflate = 1;  // DEFLATE (zlib stream) via miniz

/** metadata marker entry: its content is the production .nsd virtual path
 *  (e.g. "data/demo.nsd") so --play knows what to run. */
constexpr const char* kNspProductionMarker = ".ns-production";

/** FNV-1a 64 - lightweight non-cryptographic content hash (Phase 9).
 *  Canonical 64-bit FNV-1a: offset basis 14695981039346656037
 *  (0xcbf29ce484222325), prime 1099511628211. Verified against the
 *  reference test vectors on isthe.com/chongo/tech/comp/fnv. */
uint64_t fnv1a64(const uint8_t* data, size_t n);
inline uint64_t fnv1a64(const std::vector<uint8_t>& v) {
  return v.empty() ? 14695981039346656037ull : fnv1a64(v.data(), v.size());
}

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
class PackageWriter {
public:
  /** begin writing to outPath; the file is (re)created on finish(). */
  bool begin(const std::string& outPath, std::string* err);

  /** stage a file (replaces an existing entry with the same virtual path).
   *  The virtual path is normalized; unsafe paths are rejected. compress is
   *  a hint only: finish() DEFLATEs the payload and keeps it ONLY when the
   *  compressed form is actually smaller, so already-compressed or incompress-
   *  ible content always stays stored (method 0). The stored hash is always
   *  over the uncompressed bytes. */
  bool addFile(const std::string& vpath, const std::vector<uint8_t>& data,
               std::string* err, bool compress = false);
  bool addFile(const std::string& vpath, const std::string& text,
               std::string* err, bool compress = false);

  /** record the production .nsd virtual path; finish() stores it as the
   *  .ns-production marker entry so --play knows what to run. The path must
   *  be safe and must be a file actually added to the package. */
  bool setProduction(const std::string& vpath, std::string* err);

  /** write the header + manifest + data. Returns false with err on failure
   *  (a partially written file is removed). */
  bool finish(std::string* err);

  size_t fileCount() const { return files_.size(); }
  uint64_t totalBytes() const { return totalBytes_; }

  /** per-method write totals (raw = uncompressed source bytes, stored =
   *  bytes actually written to the package) - filled in by finish(). */
  struct MethodStats {
    size_t count = 0;
    uint64_t rawBytes = 0, storedBytes = 0;
  };
  struct WriteStats {
    MethodStats store, deflate;
  };
  const WriteStats& stats() const { return stats_; }

private:
  struct Entry {
    std::string name;
    std::vector<uint8_t> data;
    uint64_t hash = 0;
    bool compressHint = false;
  };
  std::string outPath_;
  std::map<std::string, Entry> files_;
  std::string production_;  // .ns-production marker content (virtual path)
  uint64_t totalBytes_ = 0;
  WriteStats stats_;
};

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------
class PackageReader {
public:
  /** open + fully validate the header and manifest. False with err on any
   *  structural problem (bad magic, unsupported version, truncated file,
   *  out-of-range offsets, unsafe names). */
  bool open(const std::string& path, std::string* err);

  bool isOpen() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  size_t fileCount() const { return entries_.size(); }

  bool has(const std::string& vpath) const;
  /** uncompressed size, 0 when missing */
  uint64_t fileSize(const std::string& vpath) const;
  /** storage method of an entry (kNspMethodStore / kNspMethodDeflate). */
  uint32_t method(const std::string& vpath) const;

  /** read + hash-verify a file. Empty on a missing file or checksum
   *  mismatch (lastError() names the failure). */
  std::vector<uint8_t> read(const std::string& vpath) const;
  std::string readText(const std::string& vpath) const;

  /** all virtual paths in the package (sorted) */
  std::vector<std::string> fileList() const;

  /** verify every entry's FNV-1a hash against the stored data. False with
   *  err naming the first corrupt file. */
  bool verifyAll(std::string* err) const;

  /** most recent error from read()/readText() ("" when none) */
  std::string lastError() const { return lastError_; }

  /** directory index (built at open): does a virtual directory exist? */
  bool isDirectory(const std::string& vdir) const;
  /** direct children of a virtual directory as FULL virtual paths
   *  (files and subdirs; "" = root). Empty when the dir doesn't exist. */
  std::vector<std::string> listDirectory(const std::string& vdir) const;

private:
  struct Entry {
    std::string name;
    uint64_t offset = 0;
    uint64_t packedSize = 0;
    uint64_t size = 0;
    uint32_t method = 0;
    uint64_t hash = 0;
  };
  std::string path_;
  uint64_t fileSize_ = 0;  // validated at open; read() bounds against this
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::set<std::string>> dirs_;  // parent -> children
  mutable std::string lastError_;
  // no whole-package buffer: open() keeps the header + manifest only, and
  // read() opens the file and seeks to each entry's offset on demand.
};

}  // namespace ns
