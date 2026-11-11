
// The OLE virtual file source: descriptor parsing and delayed reads from an// in-process IDataObject. The clipboard handoff itself (OleGetClipboard) needs
// a real third-party source and is exercised by hand, not here.

#include "chast/clipboard_oledata.h"

#include "chast/clipboard_virtual.h"
#include "chast/protocol.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

CLIPFORMAT descriptor_format() {
    static const CLIPFORMAT format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    return format;
}

CLIPFORMAT contents_format() {
    static const CLIPFORMAT format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    return format;
}

struct DescriptorSpec {
    std::wstring name;
    uint64_t size = 0;
    bool directory = false;
    bool size_flag = true;
    uint32_t seconds = 0;
};

std::vector<uint8_t> make_descriptor(const std::vector<DescriptorSpec>& specs) {
    std::vector<uint8_t> bytes(sizeof(DWORD) + specs.size() * sizeof(FILEDESCRIPTORW));
    const DWORD count = static_cast<DWORD>(specs.size());
    std::memcpy(bytes.data(), &count, sizeof(count));
    for (size_t i = 0; i < specs.size(); ++i) {
        FILEDESCRIPTORW item{};
        item.dwFlags = specs[i].size_flag ? FD_FILESIZE : 0;
        item.dwFileAttributes = specs[i].directory ? FILE_ATTRIBUTE_DIRECTORY
                                                   : FILE_ATTRIBUTE_NORMAL;
        item.nFileSizeHigh = static_cast<DWORD>(specs[i].size >> 32);
        item.nFileSizeLow = static_cast<DWORD>(specs[i].size);
        ULARGE_INTEGER time{};
        time.QuadPart = 116444736000000000ull + specs[i].seconds * 10000000ull;
        item.ftLastWriteTime.dwLowDateTime = time.LowPart;
        item.ftLastWriteTime.dwHighDateTime = time.HighPart;
        const size_t copied =
            (std::min)(specs[i].name.size(), static_cast<size_t>(MAX_PATH - 1));
        std::memcpy(item.cFileName, specs[i].name.data(), copied * sizeof(wchar_t));
        std::memcpy(bytes.data() + sizeof(DWORD) + i * sizeof(item), &item, sizeof(item));
    }
    return bytes;
}

class MemoryStream final : public IStream {
public:
    MemoryStream(std::vector<uint8_t> bytes, bool seekable)
        : bytes_(std::move(bytes)), seekable_(seekable) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IStream) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG length, ULONG* got) override {
        if (!got || (!data && length)) return E_POINTER;
        const size_t remaining = bytes_.size() - position_;
        const size_t take = (std::min)(remaining, static_cast<size_t>(length));
        if (take) std::memcpy(data, bytes_.data() + position_, take);
        position_ += take;
        *got = static_cast<ULONG>(take);
        return take < length ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* next) override {
        if (!seekable_) return STG_E_INVALIDFUNCTION;
        LONGLONG target = 0;
        switch (origin) {
            case STREAM_SEEK_SET: target = move.QuadPart; break;
            case STREAM_SEEK_CUR: target = static_cast<LONGLONG>(position_) + move.QuadPart; break;
            case STREAM_SEEK_END: target = static_cast<LONGLONG>(bytes_.size()) + move.QuadPart; break;
            default: return STG_E_INVALIDFUNCTION;
        }
        if (target < 0 || static_cast<uint64_t>(target) > bytes_.size()) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<size_t>(target);
        if (next) next->QuadPart = static_cast<ULONGLONG>(position_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = bytes_.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::vector<uint8_t> bytes_;
    const bool seekable_;
    size_t position_ = 0;
    std::atomic<ULONG> refs_{1};
};

class FakeDataObject final : public IDataObject {
public:
    struct File {
        std::wstring name;
        std::vector<uint8_t> bytes;
        bool directory = false;
        uint32_t seconds = 0;
    };
    explicit FakeDataObject(std::vector<File> files, bool seekable = true)
        : files_(std::move(files)), seekable_(seekable) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    ULONG refs() const { return refs_.load(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) return E_POINTER;
        if (format->cfFormat == descriptor_format() && (format->tymed & TYMED_HGLOBAL)) {
            std::vector<DescriptorSpec> specs;
            for (const File& file : files_) {
                DescriptorSpec spec;
                spec.name = file.name;
                spec.size = file.bytes.size();
                spec.directory = file.directory;
                spec.seconds = file.seconds;
                specs.push_back(spec);
            }
            const auto bytes = make_descriptor(specs);
            HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
            if (!handle) return E_OUTOFMEMORY;
            void* target = GlobalLock(handle);
            if (!target) {
                GlobalFree(handle);
                return E_OUTOFMEMORY;
            }
            std::memcpy(target, bytes.data(), bytes.size());
            GlobalUnlock(handle);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = handle;
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (format->cfFormat == contents_format() && (format->tymed & TYMED_ISTREAM)) {
            if (format->lindex < 0 ||
                static_cast<size_t>(format->lindex) >= files_.size() ||
                files_[static_cast<size_t>(format->lindex)].directory) {
                return DV_E_LINDEX;
            }
            const auto& bytes = files_[static_cast<size_t>(format->lindex)].bytes;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = new MemoryStream(bytes, seekable_);
            medium->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->cfFormat == descriptor_format() ||
            format->cfFormat == contents_format()) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
    std::vector<File> files_;
    const bool seekable_;
    std::atomic<ULONG> refs_{1};
};

std::string as_text(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

void parse_checks() {
    const auto bytes = make_descriptor({{L"notes.txt", 11, false, true, 7},
                                        {L"empty.bin", 0, false, true, 7},
                                        {L"folder", 0, true, true, 7}});
    std::vector<chast::ClipboardFileEntry> entries;
    uint64_t version = 0;
    std::string error;
    check(chast::parse_ole_file_descriptors(bytes.data(), bytes.size(), entries, version, &error),
          "a well-formed descriptor parses");
    check(entries.size() == 3 && entries[0].name == "notes.txt" && entries[0].size == 11 &&
              !entries[0].directory && entries[1].name == "empty.bin" && entries[1].size == 0 &&
              entries[2].name == "folder" && entries[2].directory && entries[2].size == 0,
          "descriptor entries keep names, sizes and folders");
    check(version != 0, "the descriptor has an identity");

    std::vector<chast::ClipboardFileEntry> again;
    uint64_t same = 0;
    check(chast::parse_ole_file_descriptors(bytes.data(), bytes.size(), again, same, &error) &&
              same == version, "the same descriptor hashes the same");

    auto changed_bytes = make_descriptor({{L"notes.txt", 11, false, true, 8},
                                          {L"empty.bin", 0, false, true, 7},
                                          {L"folder", 0, true, true, 7}});
    uint64_t changed = 0;
    check(chast::parse_ole_file_descriptors(changed_bytes.data(), changed_bytes.size(), again,
                                            changed, &error) &&
              changed != version, "a re-copy changes the identity");

    auto bad_name = make_descriptor({{L"..\\escape.txt", 1, false, true, 0}});
    check(!chast::parse_ole_file_descriptors(bad_name.data(), bad_name.size(), again, changed,
                                             &error) && !error.empty(),
          "a name with separators is refused");

    auto no_size = make_descriptor({{L"stream.bin", 0, false, false, 0}});
    check(!chast::parse_ole_file_descriptors(no_size.data(), no_size.size(), again, changed,
                                             &error),
          "a file without a size is refused");

    auto oversized = make_descriptor(
        {{L"huge.bin", chast::kMaxFileTransferBytes + 1, false, true, 0}});
    check(!chast::parse_ole_file_descriptors(oversized.data(), oversized.size(), again, changed,
                                             &error),
          "a file past the transfer cap is refused");

    check(!chast::parse_ole_file_descriptors(bytes.data(), bytes.size() - 1, again, changed,
                                             &error),
          "a truncated descriptor is refused");
    const uint8_t empty[4] = {};
    check(!chast::parse_ole_file_descriptors(empty, sizeof(empty), again, changed, &error),
          "an empty descriptor is refused");
}

std::shared_ptr<chast::ClipboardVirtualFiles> make_source(FakeDataObject& object,
                                                          std::string* error) {
    const auto bytes = make_descriptor({{L"notes.txt", 11, false, true, 7},
                                        {L"empty.bin", 0, false, true, 7},
                                        {L"folder", 0, true, true, 7}});
    std::vector<chast::ClipboardFileEntry> entries;
    uint64_t version = 0;
    if (!chast::parse_ole_file_descriptors(bytes.data(), bytes.size(), entries, version, error)) {
        return {};
    }
    object.AddRef();
    return chast::make_ole_clipboard_files(&object, entries, version, error);
}

void read_checks(bool seekable) {
    FakeDataObject object({{L"notes.txt", {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'}},
                           {L"empty.bin", {}},
                           {L"folder", {}, true}},
                          seekable);
    std::string error;
    auto files = make_source(object, &error);
    check(files != nullptr, "the data object becomes a delayed source");
    if (!files) return;
    check(files->entries().size() == 3 && files->entries()[0].size == 11,
          "the source keeps the announced entries");
    std::vector<uint8_t> bytes;
    check(files->Read(0, 0, 11, bytes, &error) && as_text(bytes) == "hello world",
          "a whole file reads through the stream");
    check(files->Read(0, 6, 5, bytes, &error) && as_text(bytes) == "world",
          "a mid-file range reads through the stream");
    check(files->Read(0, 0, 5, bytes, &error) && as_text(bytes) == "hello",
          "an earlier range still reads correctly");
    check(!files->Read(0, 0, 12, bytes, &error), "a range past the announced size is refused");
    check(!files->Read(1, 0, 1, bytes, &error), "an empty file has no readable range");
    check(!files->Read(2, 0, 1, bytes, &error), "a folder entry cannot be read");
    files->Cancel();
    check(!files->Active() && !files->Read(0, 0, 1, bytes, &error),
          "cancelled reads fail without waiting");
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    parse_checks();
    read_checks(true);
    read_checks(false);
    {
        std::string error;
        check(!chast::make_ole_clipboard_files(nullptr, {}, 0, &error) && !error.empty(),
              "a missing data object is refused");
    }
    if (failures == 0) std::fprintf(stderr, "clipboard OLE data checks passed\n");
    return failures == 0 ? 0 : 1;
}
