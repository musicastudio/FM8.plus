#include "rsrc.h"
#include <cstdint>
#include <cstring>

namespace fm8plus::Rsrc {
namespace {

struct Entry { const char* type; int id; const void* data; size_t size; };
constexpr int kMax = 32;
Entry g_entries[kMax];           // a fake HRSRC/HGLOBAL is a pointer into this array
int g_count = 0;
HMODULE g_fm8 = nullptr;

using FindResourceA_t   = HRSRC   (WINAPI*)(HMODULE, LPCSTR, LPCSTR);
using SizeofResource_t  = DWORD   (WINAPI*)(HMODULE, HRSRC);
using LoadResource_t    = HGLOBAL (WINAPI*)(HMODULE, HRSRC);
using LockResource_t    = LPVOID  (WINAPI*)(HGLOBAL);
FindResourceA_t  o_find = nullptr;
SizeofResource_t o_size = nullptr;
LoadResource_t   o_load = nullptr;
LockResource_t   o_lock = nullptr;

Entry* ours(const void* h) {
    return (h >= g_entries && h < g_entries + g_count) ? (Entry*)h : nullptr;
}

HRSRC WINAPI h_find(HMODULE mod, LPCSTR name, LPCSTR type) {
    if (mod == g_fm8 && IS_INTRESOURCE(name) && !IS_INTRESOURCE(type)) {
        const int id = (int)(uintptr_t)name;
        for (int i = 0; i < g_count; ++i)
            if (g_entries[i].id == id && _stricmp(type, g_entries[i].type) == 0) return (HRSRC)&g_entries[i];
    }
    return o_find(mod, name, type);
}
DWORD   WINAPI h_size(HMODULE mod, HRSRC r) { Entry* e = ours(r); return e ? (DWORD)e->size : o_size(mod, r); }
HGLOBAL WINAPI h_load(HMODULE mod, HRSRC r) { return ours(r) ? (HGLOBAL)r : o_load(mod, r); }
LPVOID  WINAPI h_lock(HGLOBAL g)            { Entry* e = ours(g); return e ? (LPVOID)e->data : o_lock(g); }

} // namespace

void* patchImport(HMODULE mod, const char* dll, const char* func, void* replacement) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto* thunks = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++thunks) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (std::strcmp(byName->Name, func) != 0) continue;
            DWORD old; void* orig = (void*)thunks->u1.Function;
            if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
            thunks->u1.Function = (ULONGLONG)replacement;
            VirtualProtect(&thunks->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

bool install(HMODULE fm8) {
    if (g_fm8) return g_fm8 == fm8;
    const char* k32 = "KERNEL32.dll";
    o_find = (FindResourceA_t)patchImport(fm8, k32, "FindResourceA", (void*)&h_find);
    o_size = (SizeofResource_t)patchImport(fm8, k32, "SizeofResource", (void*)&h_size);
    o_load = (LoadResource_t)patchImport(fm8, k32, "LoadResource", (void*)&h_load);
    o_lock = (LockResource_t)patchImport(fm8, k32, "LockResource", (void*)&h_lock);
    if (!o_find || !o_size || !o_load || !o_lock) return false;   // ponytail: partial patch is left as is; FM8 imports all four
    g_fm8 = fm8;
    return true;
}

void serve(const char* type, int id, const void* data, size_t size) {
    for (int i = 0; i < g_count; ++i)
        if (g_entries[i].id == id && _stricmp(type, g_entries[i].type) == 0) {
            g_entries[i] = {type, id, data, size};
            return;
        }
    if (g_count < kMax) g_entries[g_count++] = {type, id, data, size};
}

} // namespace fm8plus::Rsrc
