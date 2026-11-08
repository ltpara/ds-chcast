#define NOMINMAX
#include "chast/clipboard_virtual.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace chast {
namespace {
constexpr uint32_t kReadBlock = 64 * 1024;
UINT descriptor_format() { static const UINT id = RegisterClipboardFormatW(L"FileGroupDescriptorW"); return id; }
UINT contents_format() { static const UINT id = RegisterClipboardFormatW(L"FileContents"); return id; }
UINT effect_format() { static const UINT id = RegisterClipboardFormatW(L"Preferred DropEffect"); return id; }
UINT marker_format() { static const UINT id = RegisterClipboardFormatW(L"Chast.VirtualClipboard"); return id; }

HRESULT global_data(const void* data, size_t size, STGMEDIUM* medium) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return E_OUTOFMEMORY;
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return E_OUTOFMEMORY; }
    std::memcpy(target, data, size);
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
}

class FileStream final : public IStream {
public:
    FileStream(std::shared_ptr<ClipboardVirtualFiles> files, size_t index, uint64_t offset = 0)
        : files_(std::move(files)), index_(index), offset_(offset) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ISequentialStream && iid != IID_IStream) return E_NOINTERFACE;
        *out = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_; if (!refs) delete this; return refs;
    }
    HRESULT STDMETHODCALLTYPE Read(void* out, ULONG count, ULONG* read) override try {
        if (read) *read = 0;
        if (count && !out) return STG_E_INVALIDPOINTER;
        std::lock_guard lock(mutex_);
        if (!files_->Active()) return STG_E_REVERTED;
        const auto size = files_->entries()[index_].size;
        ULONG done = 0;
        while (done < count && offset_ < size) {
            const auto want = static_cast<uint32_t>(std::min<uint64_t>({count - done, size - offset_, kReadBlock}));
            std::vector<uint8_t> bytes;
            std::string error;
            if (!files_->Read(index_, offset_, want, bytes, &error) || !files_->Active() || bytes.size() != want) {
                if (read) *read = done;
                return STG_E_READFAULT;
            }
            std::memcpy(static_cast<uint8_t*>(out) + done, bytes.data(), bytes.size());
            done += want; offset_ += want;
        }
        if (read) *read = done;
        return done == count ? S_OK : S_FALSE;
    } catch (...) {
        if (read) *read = 0;
        return STG_E_READFAULT;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* written) override {
        if (written) *written = 0; return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        std::lock_guard lock(mutex_);
        if (!files_->Active()) return STG_E_REVERTED;
        const uint64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? offset_ :
            origin == STREAM_SEEK_END ? files_->entries()[index_].size : UINT64_MAX;
        if (base == UINT64_MAX || base > INT64_MAX) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart < 0) {
            const uint64_t magnitude = static_cast<uint64_t>(-(move.QuadPart + 1)) + 1;
            if (magnitude > base) return STG_E_INVALIDFUNCTION;
            offset_ = base - magnitude;
        } else {
            if (static_cast<uint64_t>(move.QuadPart) > INT64_MAX - base) return STG_E_INVALIDFUNCTION;
            offset_ = base + static_cast<uint64_t>(move.QuadPart);
        }
        if (result) result->QuadPart = offset_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override try {
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        if (!target) return STG_E_INVALIDPOINTER;
        std::vector<uint8_t> buffer(kReadBlock);
        uint64_t total = 0, saved = 0;
        HRESULT status = S_OK;
        while (total < count.QuadPart) {
            ULONG got = 0, put = 0;
            status = Read(buffer.data(), static_cast<ULONG>(std::min<uint64_t>(buffer.size(), count.QuadPart - total)), &got);
            total += got;
            if (got) {
                const HRESULT sent = target->Write(buffer.data(), got, &put);
                saved += put;
                if (FAILED(sent) || put != got) { status = FAILED(sent) ? sent : STG_E_MEDIUMFULL; break; }
            }
            if (status != S_OK || !got) break;
        }
        if (read) read->QuadPart = total;
        if (written) written->QuadPart = saved;
        return status;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME) return STG_E_INVALIDFLAG;
        *stat = {};
        if (!files_->Active()) return STG_E_REVERTED;
        stat->type = STGTY_STREAM; stat->grfMode = STGM_READ;
        stat->cbSize.QuadPart = files_->entries()[index_].size;
        if (flags == STATFLAG_DEFAULT) {
            const auto& name = files_->entries()[index_].name;
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
            if (size <= 0) return STG_E_INVALIDNAME;
            stat->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc((size + 1) * sizeof(wchar_t)));
            if (!stat->pwcsName) return E_OUTOFMEMORY;
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), stat->pwcsName, size);
            stat->pwcsName[size] = 0;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return E_POINTER;
        std::lock_guard lock(mutex_);
        *out = new (std::nothrow) FileStream(files_, index_, offset_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ClipboardVirtualFiles> files_;
    size_t index_;
    uint64_t offset_;
    std::mutex mutex_;
};

struct Descriptor { FILEDESCRIPTORW value{}; size_t index = SIZE_MAX; };
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool descriptors(const std::vector<ClipboardFileEntry>& entries, std::vector<Descriptor>& out) {
    if (encode_clipboard_file_list(entries).empty()) return false;
    std::map<std::wstring, size_t, NameLess> known;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto& path = entry.rel_path.empty() ? entry.name : entry.rel_path;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
        if (length <= 0 || length >= MAX_PATH || entry.size > INT64_MAX) return false;
        std::wstring wide(static_cast<size_t>(length), 0);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), length);
        std::replace(wide.begin(), wide.end(), L'/', L'\\');
        size_t begin = 0;
        for (;;) {
            const size_t end = wide.find(L'\\', begin);
            const bool leaf = end == std::wstring::npos;
            const auto part = wide.substr(begin, leaf ? wide.size() - begin : end - begin);
            if (part.empty() || part.back() == L'.' || part.back() == L' ' ||
                part.find_first_of(L"<>:\"/|?*") != std::wstring::npos ||
                std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32; })) return false;
            const auto base = part.substr(0, part.find(L'.'));
            if (_wcsicmp(base.c_str(), L"CON") == 0 || _wcsicmp(base.c_str(), L"PRN") == 0 ||
                _wcsicmp(base.c_str(), L"AUX") == 0 || _wcsicmp(base.c_str(), L"NUL") == 0 ||
                (base.size() == 4 && (_wcsnicmp(base.c_str(), L"COM", 3) == 0 || _wcsnicmp(base.c_str(), L"LPT", 3) == 0) &&
                 base[3] >= L'1' && base[3] <= L'9')) return false;
            const auto prefix = wide.substr(0, leaf ? wide.size() : end);
            const bool directory = !leaf || entry.directory;
            const auto found = known.find(prefix);
            if (found != known.end()) {
                if (leaf || (out[found->second].value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
            } else {
                Descriptor descriptor;
                descriptor.value.dwFlags = static_cast<DWORD>(FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI);
                descriptor.value.dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                if (!directory) {
                    descriptor.value.dwFlags |= FD_FILESIZE;
                    descriptor.value.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
                    descriptor.value.nFileSizeLow = static_cast<DWORD>(entry.size);
                    descriptor.index = i;
                }
                std::copy(prefix.begin(), prefix.end(), descriptor.value.cFileName);
                known.emplace(prefix, out.size());
                out.push_back(descriptor);
            }
            if (leaf) break;
            begin = end + 1;
        }
    }
    return !out.empty();
}

class FileDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    FileDataObject(std::shared_ptr<ClipboardVirtualFiles> files, std::vector<Descriptor> descriptors)
        : files_(std::move(files)), descriptors_(std::move(descriptors)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) *out = static_cast<IDataObject*>(this);
        else if (iid == IID_IDataObjectAsyncCapability) *out = static_cast<IDataObjectAsyncCapability*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (!files_->Active()) return STG_E_REVERTED;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->cfFormat == contents_format()) {
            if (!(format->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
            // Shell probes format availability before choosing an item index.
            if (format->lindex == -1) return S_OK;
            if (format->lindex < 0 || static_cast<size_t>(format->lindex) >= descriptors_.size() ||
                descriptors_[format->lindex].index == SIZE_MAX) return DV_E_LINDEX;
        } else if (format->cfFormat == descriptor_format() || format->cfFormat == effect_format() || format->cfFormat == marker_format()) {
            if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            if (format->lindex != -1) return DV_E_LINDEX;
        } else return DV_E_FORMATETC;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override try {
        if (!medium) return E_POINTER;
        *medium = {};
        const HRESULT query = QueryGetData(format);
        if (FAILED(query)) return query;
        if (format->cfFormat == contents_format()) {
            if (format->lindex < 0) return DV_E_LINDEX;
            medium->pstm = new (std::nothrow) FileStream(files_, descriptors_[format->lindex].index);
            if (!medium->pstm) return E_OUTOFMEMORY;
            medium->tymed = TYMED_ISTREAM; return S_OK;
        }
        if (format->cfFormat == descriptor_format()) {
            const size_t size = offsetof(FILEGROUPDESCRIPTORW, fgd) + descriptors_.size() * sizeof(FILEDESCRIPTORW);
            std::vector<uint8_t> data(size);
            auto* group = reinterpret_cast<FILEGROUPDESCRIPTORW*>(data.data());
            group->cItems = static_cast<UINT>(descriptors_.size());
            for (size_t i = 0; i < descriptors_.size(); ++i) group->fgd[i] = descriptors_[i].value;
            return global_data(data.data(), data.size(), medium);
        }
        const DWORD value = DROPEFFECT_COPY;
        return global_data(&value, sizeof(value), medium);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER; out->ptd = nullptr; return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_POINTER;
        // Shell's completion notifications are advisory. A copy never deletes
        // the remote source, including when a target asks for move semantics.
        if (format->cfFormat != RegisterClipboardFormatW(L"Performed DropEffect") &&
            format->cfFormat != RegisterClipboardFormatW(L"Paste Succeeded")) return DV_E_FORMATETC;
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[] = {
            {static_cast<CLIPFORMAT>(descriptor_format()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {static_cast<CLIPFORMAT>(contents_format()), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
            {static_cast<CLIPFORMAT>(effect_format()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {static_cast<CLIPFORMAT>(marker_format()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(std::size(formats)), formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override { if (out) *out = nullptr; return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { async_ = value != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* out) override { if (!out) return E_POINTER; *out = async_; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { if (!files_->Active()) return STG_E_REVERTED; operating_ = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* out) override { if (!out) return E_POINTER; *out = operating_; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override { operating_ = false; return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<ClipboardVirtualFiles> files_;
    std::vector<Descriptor> descriptors_;
    bool async_ = true;
    bool operating_ = false;
};
} // namespace

IDataObject* make_virtual_file_data_object(std::shared_ptr<ClipboardVirtualFiles> files, std::string* error) {
    try {
        std::vector<Descriptor> list;
        if (files && files->Active() && descriptors(files->entries(), list))
            return new (std::nothrow) FileDataObject(std::move(files), std::move(list));
    } catch (...) {}
    if (error) *error = "the remote file names cannot be represented by Windows Shell descriptors";
    return nullptr;
}

struct VirtualClipboard::State {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HDESK desktop = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<ClipboardVirtualFiles> pending;
    std::shared_ptr<ClipboardVirtualFiles> current;
    uint64_t generation = 0, completed = 0;
    bool stop = false, success = false;
    ~State() { if (event) CloseHandle(event); if (desktop) CloseDesktop(desktop); }
};

VirtualClipboard::VirtualClipboard() : state_(std::make_shared<State>()) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &needed))
        state_->desktop = OpenDesktopW(name, 0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
    std::thread([state = state_] {
        const HRESULT initialized = state->desktop && SetThreadDesktop(state->desktop) ? OleInitialize(nullptr) : E_ACCESSDENIED;
        IDataObject* object = nullptr;
        uint64_t applied = 0;
        for (;;) {
            std::shared_ptr<ClipboardVirtualFiles> files, old;
            uint64_t generation;
            bool stop;
            {
                std::lock_guard lock(state->mutex);
                stop = state->stop; generation = state->generation;
                if (stop || generation != applied) { old = std::move(state->current); files = state->pending; }
            }
            if (stop || generation != applied) {
                if (old) old->Cancel();
                if (object) {
                    if (OleIsCurrentClipboard(object) == S_OK) OleSetClipboard(nullptr);
                    object->Release(); object = nullptr;
                }
                if (stop) break;
                bool ok = !files;
                if (files && SUCCEEDED(initialized)) {
                    auto* next = make_virtual_file_data_object(files, nullptr);
                    if (next) {
                        ok = SUCCEEDED(OleSetClipboard(next));
                        if (ok) object = next; else next->Release();
                    }
                }
                {
                    std::lock_guard lock(state->mutex);
                    state->success = ok; state->completed = generation;
                    if (ok) state->current = files;
                    state->cv.notify_all();
                }
                applied = generation;
            }
            std::shared_ptr<ClipboardVirtualFiles> active;
            { std::lock_guard lock(state->mutex); active = state->current; }
            if (object && ((!active || !active->Active()) || OleIsCurrentClipboard(object) != S_OK)) {
                if (OleIsCurrentClipboard(object) == S_OK) OleSetClipboard(nullptr);
                std::shared_ptr<ClipboardVirtualFiles> lost;
                { std::lock_guard lock(state->mutex); lost = std::move(state->current); }
                if (lost) lost->Cancel();
                object->Release(); object = nullptr;
            }
            if (!state->event) break;
            MsgWaitForMultipleObjects(1, &state->event, FALSE, 100, QS_ALLINPUT);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        if (SUCCEEDED(initialized)) OleUninitialize();
    }).detach();
}

VirtualClipboard::~VirtualClipboard() {
    Clear();
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    SetEvent(state_->event);
}

void VirtualClipboard::Clear() {
    std::shared_ptr<ClipboardVirtualFiles> pending, current;
    {
        std::lock_guard lock(state_->mutex);
        pending = std::move(state_->pending); current = state_->current;
        ++state_->generation; SetEvent(state_->event);
    }
    if (pending) pending->Cancel();
    if (current) current->Cancel();
}

bool VirtualClipboard::Publish(std::shared_ptr<ClipboardVirtualFiles> files, std::string* error) {
    if (!files || !files->Active()) { if (error) *error = "the remote clipboard offer expired"; return false; }
    std::shared_ptr<ClipboardVirtualFiles> old;
    std::unique_lock lock(state_->mutex);
    if (state_->stop || !state_->event) return false;
    old = std::move(state_->pending);
    state_->pending = files;
    const auto generation = ++state_->generation;
    SetEvent(state_->event);
    lock.unlock();
    if (old && old != files) old->Cancel();
    lock.lock();
    const bool ready = state_->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state_->completed >= generation || state_->stop; });
    const bool ok = ready && !state_->stop && state_->completed == generation && state_->success;
    if (!ok) {
        const bool expired = !ready || state_->stop || state_->completed != generation;
        if (expired && state_->generation == generation) { state_->pending.reset(); ++state_->generation; SetEvent(state_->event); }
        lock.unlock();
        if (expired) files->Cancel();
        if (error) *error = "cannot publish the remote file descriptors";
    }
    return ok;
}

bool is_virtual_clipboard() { return IsClipboardFormatAvailable(marker_format()) != FALSE; }
} // namespace chast
