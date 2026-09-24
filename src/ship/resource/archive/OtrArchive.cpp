#ifdef INCLUDE_MPQ_SUPPORT

#include "ship/resource/archive/OtrArchive.h"

#include "ship/Context.h"
#include "ship/utils/filesystemtools/FileHelper.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"

#include "spdlog/spdlog.h"

namespace Ship {
OtrArchive::OtrArchive(const std::string& archivePath) : Archive(archivePath) {
    mHandle = nullptr;
}

OtrArchive::~OtrArchive() {
    SPDLOG_TRACE("destruct otrarchive: {}", GetPath());
}

static constexpr size_t kMaxMpqHandles = 4; // QuestShip: parallel readers per archive

HANDLE OtrArchive::AcquireHandle() {
    std::unique_lock<std::mutex> lock(mPoolMutex);
    for (;;) {
        if (!mFreeHandles.empty()) {
            HANDLE h = mFreeHandles.back();
            mFreeHandles.pop_back();
            return h;
        }
        if (mAllHandles.size() < kMaxMpqHandles) {
            HANDLE h = nullptr;
            if (SFileOpenArchive(GetPath().c_str(), 0, MPQ_OPEN_READ_ONLY, &h)) {
                mAllHandles.push_back(h);
                return h;
            }
            if (mAllHandles.empty()) {
                return nullptr;
            }
            // Couldn't open another; wait for one to come back.
        }
        mPoolCv.wait(lock, [this] { return !mFreeHandles.empty(); });
    }
}

void OtrArchive::ReleaseHandle(HANDLE handle) {
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        mFreeHandles.push_back(handle);
    }
    mPoolCv.notify_one();
}

std::shared_ptr<File> OtrArchive::LoadFile(const std::string& filePath) {
    if (mHandle == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from mpq archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    HANDLE archive = AcquireHandle();
    if (archive == nullptr) {
        return nullptr;
    }
    struct HandleReturn {
        OtrArchive* self;
        HANDLE h;
        ~HandleReturn() {
            self->ReleaseHandle(h);
        }
    } handleReturn{ this, archive };

    HANDLE fileHandle;
    bool attempt = SFileOpenFileEx(archive, filePath.c_str(), 0, &fileHandle);
    if (!attempt) {
        SPDLOG_TRACE("({}) Failed to open file {} from mpq archive  {}.", GetLastError(), filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    DWORD fileSize = SFileGetFileSize(fileHandle, 0);
    if (fileSize == 0) {
        SPDLOG_TRACE("({}) Failed to load file {}; filesize 0", GetLastError(), filePath, GetPath());
        return nullptr;
    }
    DWORD readBytes;
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(fileSize);
    bool readFileSuccess = SFileReadFile(fileHandle, fileToLoad->Buffer->data(), fileSize, &readBytes, NULL);

    if (!readFileSuccess) {
        SPDLOG_ERROR("({}) Failed to read file {} from mpq archive {}", GetLastError(), filePath, GetPath());
        bool closeFileSuccess = SFileCloseFile(fileHandle);
        if (!closeFileSuccess) {
            SPDLOG_ERROR("({}) Failed to close file {} from mpq after read failure in archive {}", GetLastError(),
                         filePath, GetPath());
        }
        return nullptr;
    }

    bool closeFileSuccess = SFileCloseFile(fileHandle);
    if (!closeFileSuccess) {
        SPDLOG_ERROR("({}) Failed to close file {} from mpq archive {}", GetLastError(), filePath, GetPath());
    }

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

std::shared_ptr<File> OtrArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

bool OtrArchive::Open() {
    const bool opened = SFileOpenArchive(GetPath().c_str(), 0, MPQ_OPEN_READ_ONLY, &mHandle);
    if (opened) {
        SPDLOG_INFO("Opened mpq file \"{}\"", GetPath());
        std::lock_guard<std::mutex> lock(mPoolMutex);
        mAllHandles.push_back(mHandle);
        mFreeHandles.push_back(mHandle);
    } else {
        SPDLOG_ERROR("Failed to load mpq file \"{}\"", GetPath());
        mHandle = nullptr;
        return false;
    }

    // Generate the file list by reading the list file.
    // This can also be done via the StormLib API, but this was copied from the LUS1.x implementation in GenerateCrcMap.
    auto listFile = LoadFile("(listfile)");

    // Use std::string_view to avoid unnecessary string copies
    std::vector<std::string_view> lines =
        StringHelper::Split(std::string_view(listFile->Buffer->data(), listFile->Buffer->size()), "\n");

    for (size_t i = 0; i < lines.size(); i++) {
        // Use std::string_view to avoid unnecessary string copies
        std::string_view line = lines[i].substr(0, lines[i].length() - 1); // Trim \r
        std::string lineStr = std::string(line);

        IndexFile(lineStr);
    }

    return opened;
}

bool OtrArchive::Close() {
    bool closed = true;
    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (HANDLE h : mAllHandles) {
        if (!SFileCloseArchive(h)) {
            SPDLOG_ERROR("({}) Failed to close mpq {}", GetLastError(), h);
            closed = false;
        }
    }
    mAllHandles.clear();
    mFreeHandles.clear();
    mHandle = nullptr;
    return closed;
}

bool OtrArchive::WriteFile(const std::string& filename, const std::vector<uint8_t>& data) {
    SPDLOG_INFO("otr does not support WriteFile, please use an o2r instead");
    return false;
}

} // namespace Ship

#endif // INCLUDE_MPQ_SUPPORT
