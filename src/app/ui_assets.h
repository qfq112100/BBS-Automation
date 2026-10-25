#pragma once
// All downloaded assets stay in memory; never write a font/image or install a system font.
#include <windows.h>

#include <compressapi.h>
#include <gdiplus.h>
#include <memory>
#include <objidl.h> // GDI+ requires PROPID/COM declarations, independent of service transitive includes.
#include <objidl.h>
#include <string>
#include <vector>

namespace nativeui {
class UiAssets {
    struct FontRegistration {
        std::vector<BYTE> bytes;
        HANDLE handle = nullptr;
        DWORD faces = 0;
        Gdiplus::Status collectionStatus = Gdiplus::FontFamilyNotFound;
        ~FontRegistration() {
            if (handle)
                RemoveFontMemResourceEx(handle);
        }
    };
    // Reverse destruction: image -> collection -> GDI registrations -> owned font bytes.
    FontRegistration regularRegistration, boldRegistration, iconRegistration;
    Gdiplus::PrivateFontCollection fonts;
    IStream* imageStream = nullptr;
    std::unique_ptr<Gdiplus::Image> hero;
    IStream *logoLightStream = nullptr, *logoDarkStream = nullptr;
    std::unique_ptr<Gdiplus::Image> logoLight, logoDark;
    std::wstring family = L"Microsoft YaHei UI", iconFamily;
    bool regular = false, bold = false, icons = false;
    bool RegisterFont(HINSTANCE module, int id, FontRegistration& registration) {
        if (!LoadBytes(module, id, registration.bytes))
            return false;
        registration.collectionStatus =
            fonts.AddMemoryFont(registration.bytes.data(), static_cast<INT>(registration.bytes.size()));
        registration.handle =
            AddFontMemResourceEx(registration.bytes.data(), static_cast<DWORD>(registration.bytes.size()),
                                 nullptr, &registration.faces);
        return registration.collectionStatus == Gdiplus::Ok && registration.handle && registration.faces;
    }

  public:
    UiAssets() = default;
    UiAssets(const UiAssets&) = delete;
    UiAssets& operator=(const UiAssets&) = delete;
    ~UiAssets() {
        hero.reset();
        logoLight.reset();
        logoDark.reset();
        for (IStream* stream : {imageStream, logoLightStream, logoDarkStream})
            if (stream)
                stream->Release();
    }
    // Raw resources and optional LBAP v1/LZMS packed resources share one bounded decoder.
    static bool LoadBytes(HINSTANCE module, int id, std::vector<BYTE>& output) {
        output.clear();
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource)
            return false;
        DWORD size = SizeofResource(module, resource);
        HGLOBAL loaded = LoadResource(module, resource);
        const BYTE* bytes = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        constexpr DWORD limit = 32u * 1024u * 1024u;
        if (!bytes || !size || size > limit)
            return false;
        DWORD header[4]{};
        if (size >= sizeof(header))
            CopyMemory(header, bytes, sizeof(header));
        if (header[0] != 0x5041424c) {
            output.assign(bytes, bytes + size);
            return true;
        }
        if (header[1] != 1 || header[2] != COMPRESS_ALGORITHM_LZMS || !header[3] || header[3] > limit ||
            size <= sizeof(header))
            return false;
        DECOMPRESSOR_HANDLE decoder = nullptr;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decoder))
            return false;
        struct DecoderScope {
            DECOMPRESSOR_HANDLE handle;
            ~DecoderScope() {
                CloseDecompressor(handle);
            }
        } scope{decoder};
        output.resize(header[3]);
        SIZE_T written = 0;
        bool ok = Decompress(decoder, const_cast<BYTE*>(bytes + sizeof(header)), size - sizeof(header),
                             output.data(), output.size(), &written) &&
                  written == output.size();
        if (!ok)
            output.clear();
        return ok;
    }
    // GDI+ reads the stream lazily: the stream must outlive the image, so both stay owned here.
    static bool LoadImage(HINSTANCE module, int id, IStream*& stream,
                          std::unique_ptr<Gdiplus::Image>& image) {
        std::vector<BYTE> bytes;
        if (!LoadBytes(module, id, bytes))
            return false;
        HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!storage)
            return false;
        void* destination = GlobalLock(storage);
        if (!destination) {
            GlobalFree(storage);
            return false;
        }
        CopyMemory(destination, bytes.data(), bytes.size());
        GlobalUnlock(storage);
        if (FAILED(CreateStreamOnHGlobal(storage, TRUE, &stream))) {
            GlobalFree(storage);
            return false;
        }
        image.reset(Gdiplus::Image::FromStream(stream, FALSE));
        if (!image || image->GetLastStatus() != Gdiplus::Ok || !image->GetWidth() || !image->GetHeight()) {
            image.reset();
            stream->Release();
            stream = nullptr;
            return false;
        }
        return true;
    }
    void Load(HINSTANCE module) {
        regular = RegisterFont(module, 202, regularRegistration);
        bold = RegisterFont(module, 203, boldRegistration);
        icons = RegisterFont(module, 204, iconRegistration);
        {
            INT count = fonts.GetFamilyCount(), found = 0;
            if (count > 0) {
                std::unique_ptr<Gdiplus::FontFamily[]> families(new Gdiplus::FontFamily[count]);
                if (fonts.GetFamilies(count, families.get(), &found) == Gdiplus::Ok) {
                    for (INT i = 0; i < found; ++i) {
                        wchar_t name[LF_FACESIZE]{};
                        if (families[i].GetFamilyName(name) == Gdiplus::Ok && regular &&
                            std::wstring(name).find(L"Noto") != std::wstring::npos) {
                            family = name;
                        }
                        if (std::wstring(name).find(L"Awesome") != std::wstring::npos)
                            iconFamily = name;
                    }
                }
            }
        }
        // Icons are drawn with GDI, not GDI+: its private-collection enumeration is not a GDI
        // registration test (FA6 can be omitted by GDI+ while AddFontMemResourceEx succeeds).
        // Use the downloaded font's verified legacy family and its actual 900 weight.
        icons = iconRegistration.handle && iconRegistration.faces;
        if (icons)
            iconFamily = L"Font Awesome 6 Free Solid";
        LoadImage(module, 206, logoLightStream, logoLight);
        LoadImage(module, 207, logoDarkStream, logoDark);
        std::vector<BYTE> imageBytes;
        if (!LoadBytes(module, 201, imageBytes))
            return;
        HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, imageBytes.size());
        if (!storage)
            return;
        void* destination = GlobalLock(storage);
        if (!destination) {
            GlobalFree(storage);
            return;
        }
        CopyMemory(destination, imageBytes.data(), imageBytes.size());
        GlobalUnlock(storage);
        if (FAILED(CreateStreamOnHGlobal(storage, TRUE, &imageStream))) {
            GlobalFree(storage);
            return;
        }
        // GDI+ lazily reads IStream: retain it until after Image destruction, even across repaint/capture.
        hero.reset(Gdiplus::Image::FromStream(imageStream, FALSE));
        if (!hero || hero->GetLastStatus() != Gdiplus::Ok || !hero->GetWidth() || !hero->GetHeight())
            hero.reset();
    }
    const wchar_t* TextFamily() const {
        return family.c_str();
    }
    const wchar_t* IconFamily() const {
        return iconFamily.c_str();
    }
    bool HasIcons() const {
        return icons;
    }
    Gdiplus::Image* Hero() const {
        return hero.get();
    }
    // Theme-adapted brand mark: the darker variant reads better on near-black surfaces.
    Gdiplus::Image* Logo(bool dark) const {
        Gdiplus::Image* image = dark ? logoDark.get() : logoLight.get();
        return image ? image : (dark ? logoLight.get() : logoDark.get());
    }
    std::wstring Diagnostics() const {
        return L"内存素材：hero=" + std::wstring(hero ? L"OK" : L"fallback") + L" regular=" +
               (regular ? L"OK" : L"fallback") + L" bold=" + (bold ? L"OK" : L"fallback") + L" icons=" +
               (icons ? L"OK" : L"fallback") + L" family=" + family + L" icon-family=" + iconFamily +
               L" icons-gdiplus=" + std::to_wstring((int)iconRegistration.collectionStatus) +
               L" icons-gdi-faces=" + std::to_wstring(iconRegistration.faces) + L" icons-bytes=" +
               std::to_wstring(iconRegistration.bytes.size());
    }
};
} // namespace nativeui
