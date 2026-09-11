// FM8.plus launcher (FM8.plus.exe). Installed next to the real FM8.exe (with a co-located
// FM8.plus.dll). Starts the untouched FM8.exe suspended, injects FM8.plus.dll (which attaches the
// standalone features from its DllMain), then resumes. We only ADD files to FM8's folder; no NI file
// is modified, renamed, or replaced, so plain FM8.exe stays stock and a Native Access reinstall
// (which only overwrites FM8's own files) leaves us intact. If FM8 or the DLL is missing, or
// injection fails, FM8 still starts as plain FM8.
#include <windows.h>
#include <string>
#include <vector>

namespace {

std::wstring exeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p); auto k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

std::wstring dirOf(const std::wstring& path) {
    auto k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : path.substr(0, k);
}

bool exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// We install next to FM8.exe, so it is our sibling. Fall back to the standard location only if the
// launcher was moved away from FM8.
std::wstring findFm8Exe() {
    std::wstring sibling = exeDir() + L"\\FM8.exe";
    if (exists(sibling)) return sibling;
    return L"C:\\Program Files\\Native Instruments\\FM8\\FM8.exe";
}

void fail(const std::wstring& msg) { MessageBoxW(nullptr, msg.c_str(), L"FM8+", MB_ICONERROR | MB_OK); }

// Load FM8.plus.dll inside the target: write its path, run LoadLibraryW there, wait, free the page.
bool inject(HANDLE proc, const std::wstring& dll) {
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = ll ? CreateRemoteThread(proc, nullptr, 0, ll, remote, 0, nullptr) : nullptr;
        if (th) {
            WaitForSingleObject(th, 15000);
            DWORD code = 0; GetExitCodeThread(th, &code);   // LoadLibraryW returns the module base (nonzero) on success
            ok = code != 0;
            CloseHandle(th);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring fm8 = findFm8Exe();
    if (!exists(fm8)) { fail(L"FM8 was not found at:\n" + fm8 + L"\n\nReinstall FM8.plus to set the correct path."); return 1; }
    const std::wstring dll = exeDir() + L"\\FM8.plus.dll";
    if (!exists(dll)) { fail(L"FM8.plus.dll is missing next to the launcher."); return 1; }

    std::wstring cmd = L"\"" + fm8 + L"\"";
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
    const std::wstring fm8dir = dirOf(fm8);

    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(fm8.c_str(), cmdbuf.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, fm8dir.c_str(), &si, &pi)) {
        fail(L"Could not start FM8."); return 1;
    }

    inject(pi.hProcess, dll);      // on failure, FM8 simply runs as plain FM8
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
