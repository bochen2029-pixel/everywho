// everywho · shell.cpp — the icon (--make-icon: the design as a .ico the .rc embeds) and the
// Start Menu / desktop shortcut (--shortcut). Ported from facet; everywho's glyph is three
// rising bars, the family's silhouette turned on its side.
#include "sys.h"

#include <objidl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace everywho {

namespace {
void draw_icon_pixels(int sz, std::vector<uint32_t>& p) {
    p.assign((size_t)sz * (size_t)sz, 0xFF14141A);
    auto put = [&](int x0, int y0, int x1, int y1, uint32_t argb) {
        for (int y = (std::max)(0, y0); y < (std::min)(sz, y1); ++y)
            for (int x = (std::max)(0, x0); x < (std::min)(sz, x1); ++x) p[(size_t)y * sz + x] = argb;
    };
    const int gp = (std::max)(1, sz / 12), w = (sz - 4 * gp) / 3;
    const int bottom = sz - gp, h = sz - 2 * gp;
    put(gp, bottom - h * 2 / 5, gp + w, bottom, 0xFF5CA4EE);                       // blue, short
    put(2 * gp + w, bottom - h * 3 / 5, 2 * gp + 2 * w, bottom, 0xFFF4B442);       // amber, mid
    put(3 * gp + 2 * w, bottom - h, 3 * gp + 3 * w, bottom, 0xFF40C76A);           // green, tall
}
}  // namespace

int write_icon_file(const std::string& path) {
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    const uint16_t n = (uint16_t)(sizeof sizes / sizeof sizes[0]);
    std::vector<uint8_t> out;
    auto w16 = [&](uint16_t v) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); };
    w16(0);
    w16(1);
    w16(n);
    const size_t dir_at = out.size();
    out.resize(dir_at + 16u * n);
    for (uint16_t i = 0; i < n; ++i) {
        const int sz = sizes[i];
        std::vector<uint32_t> px;
        draw_icon_pixels(sz, px);
        const size_t start = out.size();
        const uint32_t mask_stride = (((uint32_t)sz + 31) / 32) * 4;
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof bih;
        bih.biWidth = sz;
        bih.biHeight = sz * 2;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biSizeImage = (DWORD)((uint32_t)sz * (uint32_t)sz * 4 + mask_stride * (uint32_t)sz);
        const auto* hb = reinterpret_cast<const uint8_t*>(&bih);
        out.insert(out.end(), hb, hb + sizeof bih);
        for (int y = sz - 1; y >= 0; --y) {
            const auto* row = reinterpret_cast<const uint8_t*>(px.data() + (size_t)y * (size_t)sz);
            out.insert(out.end(), row, row + (size_t)sz * 4);
        }
        out.insert(out.end(), (size_t)mask_stride * (size_t)sz, (uint8_t)0);
        const uint32_t bytes = (uint32_t)(out.size() - start), off = (uint32_t)start;
        uint8_t* e = out.data() + dir_at + 16u * i;
        e[0] = (uint8_t)(sz == 256 ? 0 : sz);
        e[1] = e[0];
        e[2] = 0;
        e[3] = 0;
        e[4] = 1; e[5] = 0;
        e[6] = 32; e[7] = 0;
        memcpy(e + 8, &bytes, 4);
        memcpy(e + 12, &off, 4);
    }
    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) {
        fprintf(stderr, "everywho: cannot write %s\n", path.c_str());
        return 1;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("everywho: wrote %s (%zu bytes, %u sizes)\n", path.c_str(), out.size(), (unsigned)n);
    return 0;
}

// The shortcut targets everywho-gui.exe when it exists (Stage 3); until then the console exe in
// watch mode, which is the live view Stage 0 has.
int make_shortcut(const std::string& where) {
    const std::wstring dir = exe_dir();
    std::wstring target = dir + L"everywho-gui.exe";
    std::wstring args = L"--gui";
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        target = exe_path();
        args = L"-w";
    }
    const bool desktop = where == "desktop";
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(desktop ? FOLDERID_Desktop : FOLDERID_Programs, 0, nullptr, &folder))) {
        fprintf(stderr, "everywho: cannot resolve the %s folder\n", desktop ? "desktop" : "Start Menu");
        return 1;
    }
    const std::wstring lnk = std::wstring(folder) + L"\\everywho.lnk";
    CoTaskMemFree(folder);
    const HRESULT hi = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
    if (SUCCEEDED(hr)) {
        sl->SetPath(target.c_str());
        sl->SetArguments(args.c_str());
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetIconLocation(exe_path().c_str(), 0);
        sl->SetDescription(L"everywho - who is touching what, right now");
        IPersistFile* pf = nullptr;
        hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk.c_str(), TRUE);
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(hi)) CoUninitialize();
    if (FAILED(hr)) {
        fprintf(stderr, "everywho: could not write %s (0x%08lx)\n", narrow(lnk).c_str(), (unsigned long)hr);
        return 1;
    }
    printf("everywho: shortcut written  %s  ->  %s %s\n  %s\n", narrow(lnk).c_str(), narrow(target).c_str(), narrow(args).c_str(),
           desktop ? "double-click it on the desktop" : "type  everywho  in the Start Menu; right-click it there to pin it to the taskbar");
    return 0;
}

}  // namespace everywho
