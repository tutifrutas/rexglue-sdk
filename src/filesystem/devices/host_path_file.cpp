/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace rex::filesystem {

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle,
                           bool root_sector_probe)
    : File(file_access, entry),
      file_handle_(std::move(file_handle)),
      root_sector_probe_(root_sector_probe) {
  if (root_sector_probe_) {
    root_sector_[0] = 'J';
    root_sector_[1] = 'o';
    root_sector_[2] = 's';
    root_sector_[3] = 'h';
    // FATX/Josh metadata is big-endian. Titles mounting the cache partition
    // expect 0x40 sectors per allocation unit and one root directory cluster.
    root_sector_[8] = 0;
    root_sector_[9] = 0;
    root_sector_[10] = 0;
    root_sector_[11] = 0x40;
    root_sector_[12] = 0;
    root_sector_[13] = 0;
    root_sector_[14] = 0;
    root_sector_[15] = 1;
  }
}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (root_sector_probe_) {
    // Xbox 360 storage probes read the fourth 0x200-byte sector (offset
    // 0x800).  The extracted game tree has no raw disk stream, but this
    // minimal virtual stream supplies the documented Josh sector while
    // leaving all normal child-file reads on HostPathFile unchanged.
    if (byte_offset >= kJoshOffset + kJoshSize) {
      if (out_bytes_read) *out_bytes_read = 0;
      return X_STATUS_END_OF_FILE;
    }
    const size_t available = kJoshOffset + kJoshSize - byte_offset;
    const size_t count = std::min(buffer.size(), available);
    std::memset(buffer.data(), 0, count);
    const size_t overlap_begin = std::max(byte_offset, kJoshOffset);
    const size_t overlap_end = std::min(byte_offset + count, kJoshOffset + kJoshSize);
    if (overlap_begin < overlap_end) {
      const size_t overlap_size = overlap_end - overlap_begin;
      std::memcpy(buffer.data() + (overlap_begin - byte_offset),
                  root_sector_.data() + (overlap_begin - kJoshOffset), overlap_size);
    }
    if (out_bytes_read) *out_bytes_read = count;
    return count == buffer.size() ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (root_sector_probe_) {
    if (byte_offset >= kJoshOffset + kJoshSize) {
      if (out_bytes_written) *out_bytes_written = 0;
      return X_STATUS_END_OF_FILE;
    }
    const size_t available = kJoshOffset + kJoshSize - byte_offset;
    const size_t count = std::min(buffer.size(), available);
    const size_t overlap_begin = std::max(byte_offset, kJoshOffset);
    const size_t overlap_end = std::min(byte_offset + count, kJoshOffset + kJoshSize);
    if (overlap_begin < overlap_end) {
      const size_t overlap_size = overlap_end - overlap_begin;
      std::memcpy(root_sector_.data() + (overlap_begin - kJoshOffset),
                  buffer.data() + (overlap_begin - byte_offset), overlap_size);
    }
    if (out_bytes_written) *out_bytes_written = count;
    return count == buffer.size() ? X_STATUS_SUCCESS : X_STATUS_END_OF_FILE;
  }

  if (file_handle_ && file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
