#define NOMINMAX

#include "chast/clipboard_oledata.h"

#include "chast/clipboard_virtual.h"
#include "chast/protocol.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace chast {
namespace {

constexpr uint64_t kOleVersionSeed = 14695981039346656037ull;
constexpr uint64_t kOleVersionPrime = 1099511628211ull;
// A source that cannot seek is only re-read from the start; refusing a large
// skip keeps one read from turning into an unbounded copy of the whole stream.
constexpr uint64_t kMaxOleDiscardBytes = 8ull * 1024ull * 1024ull;
constexpr auto kOleReadTimeout = std::chrono::seconds(20);
constexpr ULONG kOleReadChunk = 64u * 1024u;

bool wide_to_utf8(const wchar_t* data, size_t length, std::string& text) {
    if (length == 0 || length > static_cast<size_t>(INT_MAX)) return false;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                                         static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    text.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data,
                               static_cast<int>(length), text.data(), size, nullptr,
                               nullptr) == size;
}

CLIPFORMAT clipboard_format(const wchar_t* name) {
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name));
}

}  // namespace

bool parse_ole_file_descriptors(const uint8_t* data, size_t size,
                                std::vector<ClipboardFileEntry>& entries,
                                uint64_t& version, std::string* error) {
    entries.clear();
    version = kOleVersionSeed;
    auto fail = [&](const char* reason) {
        entries.clear();
        if (error) *error = reason;
        return false;
    };
    auto mix_byte = [&version](uint8_t byte) {
        version ^= byte;
        version *= kOleVersionPrime;
    };
    auto mix_bytes = [&mix_byte](const std::string& text) {
        for (const unsigned char byte : text) mix_byte(byte);
    };
    auto mix_number = [&mix_byte](uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) mix_byte(static_cast<uint8_t>(value >> (i * 8)));
    };
    if (!data || size < sizeof(DWORD)) return fail("the virtual file descriptor is truncated");
    DWORD count = 0;
    std::memcpy(&count, data, sizeof(count));
    if (count == 0) return fail("the virtual file list is empty");
    if (count > kMaxClipboardFileEntries) {
        return fail("the virtual file list is longer than a copy can carry");
    }
    const size_t header = sizeof(DWORD);
    // Trailing bytes are tolerated; a block shorter than its descriptor count
    // is not.
    if (count > (size - header) / sizeof(FILEDESCRIPTORW)) {
        return fail("the virtual file descriptor has a malformed size");
    }
    std::vector<ClipboardFileEntry> parsed;
    parsed.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        FILEDESCRIPTORW item{};
        std::memcpy(&item, data + header + i * sizeof(FILEDESCRIPTORW), sizeof(item));
        const size_t name_length =
            wcsnlen(item.cFileName, static_cast<size_t>(MAX_PATH));
        std::string name;
        if (!wide_to_utf8(item.cFileName, name_length, name) ||
            !valid_clipboard_file_name(name)) {
            return fail("a virtual file name cannot be sent");
        }
        ClipboardFileEntry entry;
        entry.name = std::move(name);
        entry.directory = (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!entry.directory) {
            if ((item.dwFlags & FD_FILESIZE) == 0) {
                return fail("a virtual file does not report its size");
            }
            const uint64_t bytes =
                (static_cast<uint64_t>(item.nFileSizeHigh) << 32) | item.nFileSizeLow;
            if (bytes > kMaxFileTransferBytes) {
                return fail("a virtual file is larger than a copy can carry");
            }
            entry.size = bytes;
        }
        mix_bytes(entry.name);
        mix_byte(entry.directory ? 1u : 0u);
        mix_number(entry.size);
        mix_number((static_cast<uint64_t>(item.ftLastWriteTime.dwHighDateTime) << 32) |
                   item.ftLastWriteTime.dwLowDateTime);
        mix_byte(static_cast<uint8_t>(item.dwFileAttributes));
        parsed.push_back(std::move(entry));
    }
    entries = std::move(parsed);
    return true;
}

namespace {

// One delayed source. The data object arrives from the clipboard thread but is
// used on this adapter's own COM apartment: the global interface table
// marshals it, and every GetData/Read happens on the reader thread. A source
// that stops responding cannot be interrupted, so Cancel() only fails the
// waiting callers; the reader releases its streams when it next wakes.
class OleFiles final : public ClipboardVirtualFiles {
public:
    struct Job {
        size_t index = 0;
        uint64_t offset = 0;
        uint32_t length = 0;
        std::vector<uint8_t> bytes;
        bool ok = false;
        bool done = false;
    };
    struct State {
        IGlobalInterfaceTable* git = nullptr;
        DWORD cookie = 0;
        IDataObject* object = nullptr;
        std::map<size_t, std::pair<IStream*, uint64_t>> streams;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<Job>> jobs;
        std::atomic<bool> active{true};
        bool initialized = false;
        bool init_ok = false;

        void run(const std::vector<ClipboardFileEntry>& entries) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            {
                std::lock_guard lock(mutex);
                if (git && cookie != 0) {
                    init_ok = SUCCEEDED(git->GetInterfaceFromGlobal(
                        cookie, IID_IDataObject, reinterpret_cast<void**>(&object))) &&
                        object != nullptr;
                }
                initialized = true;
            }
            cv.notify_all();
            if (init_ok) {
                for (;;) {
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock lock(mutex);
                        cv.wait(lock, [&] { return !active || !jobs.empty(); });
                        if (!active) break;
                        job = jobs.front();
                        jobs.pop_front();
                    }
                    const bool ok = read_one(entries, *job);
                    {
                        std::lock_guard lock(mutex);
                        job->ok = ok;
                        job->done = true;
                    }
                    cv.notify_all();
                }
            }
            for (auto& entry : streams) {
                if (entry.second.first) entry.second.first->Release();
            }
            streams.clear();
            if (object) object->Release();
            object = nullptr;
            if (git) {
                if (cookie) git->RevokeInterfaceFromGlobal(cookie);
                git->Release();
            }
            git = nullptr;
            cookie = 0;
            if (SUCCEEDED(com)) CoUninitialize();
        }

    private:
        bool read_one(const std::vector<ClipboardFileEntry>& entries, Job& job) {
            if (job.index >= entries.size() || entries[job.index].directory) return false;
            auto found = streams.find(job.index);
            IStream* stream = found != streams.end() ? found->second.first : nullptr;
            uint64_t position = found != streams.end() ? found->second.second : 0;
            auto open = [&] {
                FORMATETC format{};
                format.cfFormat = clipboard_format(L"FileContents");
                format.dwAspect = DVASPECT_CONTENT;
                format.lindex = static_cast<LONG>(job.index);
                format.tymed = TYMED_ISTREAM;
                STGMEDIUM medium{};
                if (FAILED(object->GetData(&format, &medium))) return false;
                struct Releaser {
                    STGMEDIUM* medium;
                    ~Releaser() { ReleaseStgMedium(medium); }
                } release{&medium};
                if (medium.tymed != TYMED_ISTREAM || !medium.pstm) return false;
                stream = medium.pstm;
                stream->AddRef();
                position = 0;
                streams[job.index] = {stream, position};
                return true;
            };
            auto drop = [&] {
                if (stream) stream->Release();
                streams.erase(job.index);
                stream = nullptr;
                position = 0;
            };
            if (!stream && !open()) return false;
            LARGE_INTEGER target{};
            target.QuadPart = static_cast<LONGLONG>(job.offset);
            const bool seeked = stream->Seek(target, STREAM_SEEK_SET, nullptr) == S_OK;
            if (seeked) {
                position = job.offset;
            } else {
                // A stream that cannot seek is read forward. Reopen it when the
                // requested range starts before the current position, and
                // refuse a skip large enough to approach a full reread.
                if (job.offset < position) {
                    drop();
                    if (!open()) return false;
                }
                if (job.offset - position > kMaxOleDiscardBytes) {
                    drop();
                    return false;
                }
                std::vector<uint8_t> scratch(kOleReadChunk);
                while (position < job.offset) {
                    const ULONG want = static_cast<ULONG>(std::min<uint64_t>(
                        job.offset - position, scratch.size()));
                    ULONG got = 0;
                    if (FAILED(stream->Read(scratch.data(), want, &got)) || got == 0) {
                        drop();
                        return false;
                    }
                    position += got;
                }
            }
            std::vector<uint8_t> bytes(job.length);
            size_t filled = 0;
            while (filled < bytes.size()) {
                ULONG got = 0;
                const ULONG want = static_cast<ULONG>(std::min<size_t>(
                    bytes.size() - filled, kOleReadChunk));
                if (FAILED(stream->Read(bytes.data() + filled, want, &got)) || got == 0) {
                    drop();
                    return false;
                }
                filled += got;
                position += got;
            }
            streams[job.index] = {stream, position};
            job.bytes = std::move(bytes);
            return true;
        }
    };

    OleFiles(std::shared_ptr<State> state, std::vector<ClipboardFileEntry> entries,
             uint64_t version)
        : state_(std::move(state)), entries_(std::move(entries)), version_(version) {}
    ~OleFiles() override { Cancel(); }
    const std::vector<ClipboardFileEntry>& entries() const override { return entries_; }
    bool Active() const override { return state_->active.load(); }
    uint64_t SourceVersion() const override { return version_; }
    void Cancel() override {
        state_->active = false;
        std::lock_guard lock(state_->mutex);
        for (const auto& job : state_->jobs) job->done = true;
        state_->jobs.clear();
        state_->cv.notify_all();
    }
    bool Read(size_t index, uint64_t offset, uint32_t length, std::vector<uint8_t>& bytes,
              std::string* error) override {
        bytes.clear();
        auto fail = [&](const char* reason) {
            if (error) *error = reason;
            return false;
        };
        if (!Active()) return fail("the copied item was replaced");
        if (index >= entries_.size() || entries_[index].directory) {
            return fail("the clipboard does not hold that file");
        }
        if (!length || length > kMaxClipboardFileRead || offset > entries_[index].size ||
            length > entries_[index].size - offset) {
            return fail("the requested range is outside the copied file");
        }
        auto job = std::make_shared<Job>();
        job->index = index;
        job->offset = offset;
        job->length = length;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->active) return fail("the copied item was replaced");
            state_->jobs.push_back(job);
        }
        state_->cv.notify_all();
        std::unique_lock lock(state_->mutex);
        const bool ready = state_->cv.wait_for(lock, kOleReadTimeout,
                                               [&] { return !state_->active || job->done; });
        if (!ready) {
            state_->active = false;
            state_->cv.notify_all();
            return fail("the source application did not answer the file read");
        }
        if (!state_->active || !job->done || !job->ok || job->bytes.size() != length) {
            return fail("the source application refused the file read");
        }
        bytes = std::move(job->bytes);
        return true;
    }

private:
    std::shared_ptr<State> state_;
    std::vector<ClipboardFileEntry> entries_;
    const uint64_t version_;
};

}  // namespace

std::shared_ptr<ClipboardVirtualFiles> make_ole_clipboard_files(
    IDataObject* object, const std::vector<ClipboardFileEntry>& entries,
    uint64_t version, std::string* error) {
    if (!object) {
        if (error) *error = "the clipboard offers no data object";
        return {};
    }
    if (entries.empty()) {
        if (error) *error = "the virtual file list is empty";
        return {};
    }
    auto state = std::make_shared<OleFiles::State>();
    HRESULT hr = CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IGlobalInterfaceTable,
                                  reinterpret_cast<void**>(&state->git));
    if (FAILED(hr) || !state->git) {
        state->git = nullptr;
        if (error) *error = "the clipboard cannot be handed to its reader";
        return {};
    }
    hr = state->git->RegisterInterfaceInGlobal(object, IID_IDataObject, &state->cookie);
    if (FAILED(hr)) {
        state->git->Release();
        state->git = nullptr;
        state->cookie = 0;
        if (error) *error = "the clipboard data object could not be registered";
        return {};
    }
    try {
        std::thread([state, entries] { state->run(entries); }).detach();
    } catch (...) {
        state->git->RevokeInterfaceFromGlobal(state->cookie);
        state->git->Release();
        state->git = nullptr;
        state->cookie = 0;
        if (error) *error = "the clipboard reader could not be started";
        return {};
    }
    {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->initialized; });
        if (!state->init_ok) {
            if (error) *error = "the clipboard data object could not be opened";
            return {};
        }
    }
    return std::make_shared<OleFiles>(std::move(state), entries, version);
}

ClipboardReadState read_ole_clipboard_files_state(
    std::vector<ClipboardFileEntry>& entries,
    std::shared_ptr<ClipboardVirtualFiles>& files, std::string* error) {
    entries.clear();
    files.reset();
    if (error) error->clear();
    if (is_virtual_clipboard()) return ClipboardReadState::Empty;
    if (!IsClipboardFormatAvailable(clipboard_format(L"FileGroupDescriptorW"))) {
        return ClipboardReadState::Empty;
    }
    IDataObject* object = nullptr;
    const HRESULT got = OleGetClipboard(&object);
    if (FAILED(got) || !object) {
        if (error) *error = "the clipboard cannot be opened through OLE";
        return ClipboardReadState::Failed;
    }
    struct Releaser {
        IDataObject* object;
        ~Releaser() { if (object) object->Release(); }
    } release{object};
    FORMATETC format{};
    format.cfFormat = clipboard_format(L"FileGroupDescriptorW");
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(object->GetData(&format, &medium))) {
        if (error) *error = "the clipboard file descriptor could not be read";
        return ClipboardReadState::Failed;
    }
    struct MediumReleaser {
        STGMEDIUM* medium;
        ~MediumReleaser() { ReleaseStgMedium(medium); }
    } medium_release{&medium};
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
        if (error) *error = "the clipboard file descriptor has an unexpected type";
        return ClipboardReadState::Failed;
    }
    const SIZE_T bytes = GlobalSize(medium.hGlobal);
    const auto* data = static_cast<const uint8_t*>(GlobalLock(medium.hGlobal));
    if (!data || bytes == 0) {
        if (data) GlobalUnlock(medium.hGlobal);
        if (error) *error = "the clipboard file descriptor is empty";
        return ClipboardReadState::Failed;
    }
    uint64_t version = 0;
    std::vector<ClipboardFileEntry> parsed;
    const bool ok = parse_ole_file_descriptors(data, bytes, parsed, version, error);
    GlobalUnlock(medium.hGlobal);
    if (!ok) return ClipboardReadState::Failed;
    auto created = make_ole_clipboard_files(object, parsed, version, error);
    if (!created) return ClipboardReadState::Failed;
    entries = std::move(parsed);
    files = std::move(created);
    return ClipboardReadState::Content;
}

}  // namespace chast
