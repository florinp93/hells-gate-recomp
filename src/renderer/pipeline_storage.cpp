#include "pipeline_storage.h"
#include "path_util.h"

#include <rex/logging/macros.h>

#include <xxhash.h>

#include <cstring>
#include <system_error>
#include <type_traits>

namespace renderer {
namespace {

// File: Header, then records (RecordHeader + payload), appended as they are seen.
constexpr char kMagic[8] = {'D', 'A', 'N', 'T', 'E', 'P', 'S', 'O'};
// Bump only when the record layout changes: the records carry their own
// derivation versions, and GuestDrawState lets new code derive them again.
constexpr uint32_t kFormatVersion = 1;

struct Header {
  char magic[8];
  uint32_t format_version;
  uint32_t reserved;
};

enum : uint32_t { kShaderRecord = 1, kPipelineRecord = 2 };

struct RecordHeader {
  uint32_t tag;
  uint32_t size;  // of the payload
};

// Shader payload: ShaderRecord, then `dwords` guest dwords.
struct ShaderRecord {
  uint32_t type;
  uint32_t dwords;
  uint64_t hash;
};

static_assert(std::has_unique_object_representations_v<PipelineKey>);
static_assert(std::has_unique_object_representations_v<StoredPipeline>);
static_assert(std::has_unique_object_representations_v<StoredDraw>);

Header CurrentHeader() {
  Header header{};
  std::memcpy(header.magic, kMagic, sizeof(kMagic));
  header.format_version = kFormatVersion;
  return header;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  std::FILE* file = path_util::Open(path, "rb");
  if (!file) return data;
  std::fseek(file, 0, SEEK_END);
  long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0) {
    data.resize(size_t(size));
    data.resize(std::fread(data.data(), 1, data.size(), file));
  }
  std::fclose(file);
  return data;
}

}  // namespace

PipelineStorage::PipelineStorage(std::filesystem::path path,
                                 const std::filesystem::path& shipped)
    : path_(std::move(path)) {
  std::error_code ec;
  if (!shipped.empty() && std::filesystem::exists(shipped, ec)) {
    Load(shipped);
    REXLOG_INFO("PIPELINES: {} shaders, {} pipelines in {}", shaders_.size(), pipelines_.size(),
                path_util::Utf8(shipped));
  }
  size_t shipped_shaders = shaders_.size(), shipped_pipelines = pipelines_.size();
  std::filesystem::create_directories(path_.parent_path(), ec);
  uint64_t valid = Load(path_);
  if (valid) {
    // Drop a damaged tail (a record cut short by a crash).
    if (std::filesystem::file_size(path_, ec) != valid) {
      std::filesystem::resize_file(path_, valid, ec);
    }
    file_ = path_util::Open(path_, "ab");
  } else {
    if (std::filesystem::exists(path_, ec)) {
      auto old = path_;
      old += ".old";
      std::filesystem::rename(path_, old, ec);
    }
    file_ = path_util::Open(path_, "wb");
    if (file_) {
      Header header = CurrentHeader();
      std::fwrite(&header, sizeof(header), 1, file_);
      std::fflush(file_);
    }
  }
  if (!file_) {
    REXLOG_WARN("PIPELINES: cannot write {}; shaders and pipelines are not recorded",
                path_util::Utf8(path_));
  }
  REXLOG_INFO("PIPELINES: {} more shaders, {} more pipelines recorded in {}",
              shaders_.size() - shipped_shaders, pipelines_.size() - shipped_pipelines,
              path_util::Utf8(path_));
}

PipelineStorage::~PipelineStorage() {
  if (file_) std::fclose(file_);
  REXLOG_INFO("PIPELINES: {} shaders and {} pipelines added this session", added_shaders_,
              added_pipelines_);
}

uint64_t PipelineStorage::Load(const std::filesystem::path& path) {
  std::vector<uint8_t> data = ReadFile(path);
  Header expected = CurrentHeader();
  if (data.size() < sizeof(Header) || std::memcmp(data.data(), &expected, sizeof(Header))) {
    if (!data.empty()) {
      REXLOG_WARN("PIPELINES: {} is from another version, not used", path_util::Utf8(path));
    }
    return 0;
  }
  size_t pos = sizeof(Header);
  while (pos + sizeof(RecordHeader) <= data.size()) {
    RecordHeader record;
    std::memcpy(&record, &data[pos], sizeof(record));
    size_t payload = pos + sizeof(record);
    if (record.size > data.size() - payload) break;
    const uint8_t* p = &data[payload];
    if (record.tag == kShaderRecord && record.size >= sizeof(ShaderRecord)) {
      ShaderRecord shader;
      std::memcpy(&shader, p, sizeof(shader));
      size_t bytes = size_t(shader.dwords) * 4;
      if (record.size != sizeof(shader) + bytes) break;
      if (XXH3_64bits(p + sizeof(shader), bytes) != shader.hash) break;
      if (shader_hashes_.insert(shader.hash).second) {
        StoredShader& s = shaders_.emplace_back();
        s.type = shader.type;
        s.hash = shader.hash;
        s.ucode.resize(shader.dwords);
        std::memcpy(s.ucode.data(), p + sizeof(shader), bytes);
      }
    } else if (record.tag == kPipelineRecord) {
      if (record.size != sizeof(StoredDraw)) break;
      StoredDraw draw;
      std::memcpy(&draw, p, sizeof(draw));
      if (pipeline_hashes_.insert(XXH3_64bits(&draw.pipeline, sizeof(draw.pipeline))).second) {
        pipelines_.push_back(draw);
      }
    } else {
      break;
    }
    pos = payload + record.size;
  }
  return pos;
}

void PipelineStorage::Append(uint32_t tag, const void* a, size_t a_size, const void* b,
                             size_t b_size) {
  if (!file_) return;
  RecordHeader record{tag, uint32_t(a_size + b_size)};
  std::fwrite(&record, sizeof(record), 1, file_);
  std::fwrite(a, 1, a_size, file_);
  if (b_size) std::fwrite(b, 1, b_size, file_);
  // New records are rare after the first minutes; keep them through a crash.
  std::fflush(file_);
}

void PipelineStorage::AddShader(uint32_t type, uint64_t hash, const uint32_t* ucode,
                                uint32_t dwords) {
  if (!shader_hashes_.insert(hash).second) return;
  ShaderRecord shader{type, dwords, hash};
  Append(kShaderRecord, &shader, sizeof(shader), ucode, size_t(dwords) * 4);
  ++added_shaders_;
}

void PipelineStorage::AddPipeline(const StoredDraw& draw) {
  if (!pipeline_hashes_.insert(XXH3_64bits(&draw.pipeline, sizeof(draw.pipeline))).second) return;
  Append(kPipelineRecord, &draw, sizeof(draw), nullptr, 0);
  ++added_pipelines_;
}

}  // namespace renderer
