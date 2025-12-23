#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#include <format>
#include <richedit.h>
#include <iostream>

static const bool IsLongPathAwareEnabled = []() -> bool {
    const auto proc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAreLongPathsEnabled");
    const auto addr = reinterpret_cast<uintptr_t>(proc);
    return addr && reinterpret_cast<BOOLEAN(*)()>(addr)();
}();

static std::string ConvertWideStringToUTF8(const std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string str(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), str.data(), size, nullptr, nullptr);
    return str;
} /*
static std::wstring ConvertUTF8ToWideString(const std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), wstr.data(), size);
    return wstr;
}  */
static void ConsoleLog(std::wstring_view msg = L"") { std::cout << ConvertWideStringToUTF8(msg) << std::endl; }
static void ConsoleLog(std::string_view  msg =  "") { std::cout <<                         msg  << std::endl; }


constexpr INT_PTR IDC_EDIT = 1001;

class CDropTarget : public IDropTarget
{
    LONG m_cRef = 1;
    HWND m_hWnd = nullptr;
    bool m_bAcceptFormat = false;

    static inline CLIPFORMAT m_CF_SHELLIDLIST = 0;

public:
    CDropTarget(HWND hwnd) : m_hWnd(hwnd) {
        if (m_CF_SHELLIDLIST == 0) {
            m_CF_SHELLIDLIST = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);  // "Shell IDList Array"
        }
    }
    virtual ~CDropTarget() = default;

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObject, DWORD, POINTL, DWORD* pdwEffect) override {
        FORMATETC feHDrop = {   CF_HDROP,       nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        FORMATETC feSHIDL = { m_CF_SHELLIDLIST, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        m_bAcceptFormat = (pDataObject->QueryGetData(&feHDrop) == S_OK || pDataObject->QueryGetData(&feSHIDL) == S_OK);
        *pdwEffect = m_bAcceptFormat ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = m_bAcceptFormat ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { m_bAcceptFormat = false; return S_OK; }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObject, DWORD, POINTL, DWORD* pdwEffect) override
    {
        *pdwEffect = DROPEFFECT_NONE;

        std::vector<std::wstring> files = m_CollectDroppedFiles(pDataObject);
        m_HandleFilepaths(files);

        *pdwEffect = DROPEFFECT_COPY;
        m_bAcceptFormat = false;
        return S_OK;
    }

private:
    /** IDataObject Drop event parsing to collect dropped files' paths */
    static std::vector<std::wstring> m_CollectDroppedFiles(IDataObject* pDataObject) {
        std::vector<std::wstring> files;

        FORMATETC feHDrop = {   CF_HDROP,       nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        FORMATETC feSHIDL = { m_CF_SHELLIDLIST, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg;

        /** Primary (for standard paths): CF_HDROP **/
        if (SUCCEEDED(pDataObject->GetData(&feHDrop, &stg))) {
            ConsoleLog("feHDrop");
            HDROP hDrop = (HDROP)GlobalLock(stg.hGlobal);
            if (hDrop) {
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                files.reserve(count);
                for (UINT i = 0; i < count; ++i) {
                    WCHAR szPath[MAX_PATH] = {};  // 259 (the max short path string length) + 1 (`\0` char)
                    if (DragQueryFileW(hDrop, i, szPath, MAX_PATH)) {
                        files.emplace_back(m_ResolveToLongPath(szPath));
                    }
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
        } else
        /** Fallback (for long paths): RegisterClipboardFormatW("Shell IDList Array") **/
        if (SUCCEEDED(pDataObject->GetData(&feSHIDL, &stg))) {
            ConsoleLog("feSHIDL");
            LPIDA pIda = (LPIDA)GlobalLock(stg.hGlobal);
            if (pIda) {
                LPCITEMIDLIST pidlFolder = (LPCITEMIDLIST)((LPBYTE)pIda + pIda->aoffset[0]);
                for (UINT i = 1; i <= pIda->cidl; ++i) {
                    LPCITEMIDLIST pidlItem = (LPCITEMIDLIST)((LPBYTE)pIda + pIda->aoffset[i]);
                    LPITEMIDLIST pidlFull = ILCombine(pidlFolder, pidlItem);
                    if (pidlFull) {
                        if (std::wstring path = m_GetPathFromPIDL(pidlFull); !path.empty()) {
                            files.emplace_back(std::move(path));
                        }
                        ILFree(pidlFull);
                    }
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
        }
        return files;
    }

    /** CF_HDROP 8.3 path fixer */
    static std::wstring m_ResolveToLongPath(const std::wstring& path)
    {
        if (path.empty()) {
            return path;
        }
        DWORD needed = GetLongPathNameW(path.c_str(), nullptr, 0);
        if (needed == path.size() + 1) { /* no changes */
            return path;
        }
        ConsoleLog(L"ResolveToLongPath: " + path);
        if (needed == 0) { /* error */
            return path.ends_with(L" ") ? L"\\\\?\\" + path : path;
        }
        std::wstring result(needed, L'\0'); /* else: resolve 8.3 path */
        GetLongPathNameW(path.c_str(), result.data(), needed);
        result.resize(needed - 1);
        ConsoleLog(L"GetLongPathNameW: " + result);
        return (IsLongPathAwareEnabled || result.size() < MAX_PATH /* 260 */) ? result : L"\\\\?\\" + result;
    }

    /** Memory optimized SHGetPathFromIDListEx */
    static std::wstring m_GetPathFromPIDL(LPCITEMIDLIST pidl)
    {
        ConsoleLog("m_GetPathFromPIDL");
        std::wstring result;
        for (DWORD bufferSize : { 512, 4096, 32768 }) {
            result.assign(bufferSize, L'\0');
            if (SHGetPathFromIDListEx(pidl, result.data(), bufferSize, GPFIDL_DEFAULT)) {
                result.resize(std::wcslen(result.data()));
                break;
            } else {
                result = {};
            }
        }
        if (result.ends_with(L" ") && !result.starts_with(L"\\\\?\\")) {
            result = L"\\\\?\\" + result;
        }
        return result;
    }

    void m_HandleFilepaths(std::vector<std::wstring> filepaths)
    {
        std::wstring text;
        for (const auto& filepath : filepaths) {
            m_AppendFileInfo(text, filepath);
        }
        ConsoleLog(text);

        HWND hEdit = GetDlgItem(m_hWnd, IDC_EDIT);
        SetWindowTextW(hEdit, text.empty() ? L"Drag one or more files here..." : text.c_str());
    }

    static void m_AppendFileInfo(std::wstring& text, const std::wstring& path) {
        ULARGE_INTEGER size = {{0, 0}};
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            size.LowPart = fad.nFileSizeLow; size.HighPart = fad.nFileSizeHigh;
        }
        std::wstring name = wcsrchr(path.c_str(), L'\\') ? wcsrchr(path.c_str(), L'\\') + 1 : path.c_str();
        std::wstring size_str = m_FormatFileSizeW(size.QuadPart);
        auto length = path.size();
        text += std::vformat(L"Name: \t{}\r\n"
                             L"Path: \t{}\r\n"
                             L"Size: \t{}\r\n"
                             L"Length:\t{} chars\r\n\r\n",
                             std::make_wformat_args(name, path, size_str, length));
    }

    static std::wstring m_FormatFileSizeW(ULONGLONG size) {
        if (size == 0) return L"0 bytes (0 bytes)";
        if (size < 1024) return std::format(L"{} bytes ({} bytes)", size, size);
        if (size < 1024ULL * 1024)        return std::format(L"{:.1f} KB ({} bytes)", static_cast<double>(size) / 1024, size);
        if (size < 1024ULL * 1024 * 1024) return std::format(L"{:.1f} MB ({} bytes)", static_cast<double>(size) / (1024 * 1024), size);
        return std::format(L"{:.1f} GB ({} bytes)", static_cast<double>(size) / (1024 * 1024 * 1024), size);
    }
};
LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static CDropTarget* pDrop = nullptr;
    static HWND hEdit = nullptr;

    switch (uMsg) {
        case WM_CREATE: {
            pDrop = new CDropTarget(hwnd);
            RegisterDragDrop(hwnd, pDrop);
            auto editorType = LoadLibraryW(L"Msftedit.dll") ? MSFTEDIT_CLASS : L"EDIT";
            hEdit = CreateWindowW(editorType, L"Drag one or more files here...",
                                  WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                  20, 20, 1, 1, hwnd, (HMENU)IDC_EDIT, GetModuleHandle(nullptr), nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT rect; GetClientRect(hwnd, &rect);
            rect.left += 20; rect.top += 20; rect.right -= 20; rect.bottom -= 20;
            SetWindowPos(hEdit, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
            return 0;
        }
        case WM_DESTROY: {
            if (pDrop) { RevokeDragDrop(hwnd); pDrop->Release(); pDrop = nullptr; }
            if (hEdit) { DestroyWindow(hEdit); hEdit = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    ConsoleLog((IsLongPathAwareEnabled ? "longPathAware is enabled\n" : "longPathAware is disabled\n"));

    OleInitialize(nullptr);
    const WCHAR CLASS_NAME[] = L"SimpleDropWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_NAME, L"Drag files here — WinAPI Drag & Drop",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;

    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);

    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    OleUninitialize();
    return 0;
}
