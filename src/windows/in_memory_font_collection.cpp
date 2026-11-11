#include "dyslexia_reader/windows/in_memory_font_collection.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace dyslexia_reader::windows {
namespace {

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw ReaderError(
            ReaderErrorCode::FontUnavailable,
            std::string(operation) + " failed (HRESULT " +
                std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}

const wchar_t* family_name(FontFamily family) {
    switch (family) {
    case FontFamily::Lexend:
        return L"Lexend";
    case FontFamily::AtkinsonHyperlegible:
        return L"Atkinson Hyperlegible";
    case FontFamily::OpenDyslexic:
        return L"OpenDyslexic";
    }
    throw ReaderError(ReaderErrorCode::FontUnavailable,
                      "A bundled font has an unknown family");
}

class FontLoaderRegistration final {
public:
    FontLoaderRegistration(IDWriteFactory* factory,
                           IDWriteFontFileLoader* loader)
        : factory_(factory), loader_(loader) {}

    ~FontLoaderRegistration() {
        if (registered_) {
            const HRESULT result =
                factory_->UnregisterFontFileLoader(loader_);
            if (FAILED(result)) {
                OutputDebugStringA(
                    "Dyslexia Reader: failed to unregister font loader.\n");
            }
        }
    }

    void register_loader() {
        check_hresult(factory_->RegisterFontFileLoader(loader_),
                      "IDWriteFactory::RegisterFontFileLoader");
        registered_ = true;
    }

    void transfer_to_owner() noexcept { registered_ = false; }

private:
    IDWriteFactory* factory_;
    IDWriteFontFileLoader* loader_;
    bool registered_ = false;
};

}  // namespace

InMemoryFontCollection::InMemoryFontCollection(IDWriteFactory* factory,
                                               IFontLoader& fontLoader) {
    if (factory == nullptr) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "DirectWrite factory is not available");
    }

    check_hresult(factory->QueryInterface(IID_PPV_ARGS(factory_.GetAddressOf())),
                  "QueryInterface(IDWriteFactory5)");
    check_hresult(factory_->CreateInMemoryFontFileLoader(
                      memoryLoader_.GetAddressOf()),
                  "IDWriteFactory5::CreateInMemoryFontFileLoader");

    FontLoaderRegistration registration(factory_.Get(),
                                         memoryLoader_.Get());
    registration.register_loader();

    Microsoft::WRL::ComPtr<IDWriteFontSetBuilder> fontSetBuilderBase;
    check_hresult(factory_->CreateFontSetBuilder(
                      fontSetBuilderBase.GetAddressOf()),
                  "IDWriteFactory3::CreateFontSetBuilder");
    Microsoft::WRL::ComPtr<IDWriteFontSetBuilder1> fontSetBuilder;
    check_hresult(fontSetBuilderBase.As(&fontSetBuilder),
                  "QueryInterface(IDWriteFontSetBuilder1)");

    const auto fonts = fontLoader.load_fonts();
    if (fonts.empty()) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "No bundled reading fonts were provided");
    }
    fontFiles_.reserve(fonts.size());
    std::array<bool, 3> assetFamiliesFound = {false, false, false};

    for (const auto& font : fonts) {
        switch (font.family) {
        case FontFamily::Lexend:
            assetFamiliesFound[0] = true;
            break;
        case FontFamily::AtkinsonHyperlegible:
            assetFamiliesFound[1] = true;
            break;
        case FontFamily::OpenDyslexic:
            assetFamiliesFound[2] = true;
            break;
        default:
            throw ReaderError(ReaderErrorCode::FontUnavailable,
                              "A bundled font has an unknown family");
        }
        if (font.bytes.empty() ||
            font.bytes.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<UINT32>::max())) {
            throw ReaderError(ReaderErrorCode::FontUnavailable,
                              "A bundled font has an invalid size");
        }

        Microsoft::WRL::ComPtr<IDWriteFontFile> fontFile;
        check_hresult(memoryLoader_->CreateInMemoryFontFileReference(
                          factory_.Get(), font.bytes.data(),
                          static_cast<UINT32>(font.bytes.size()), nullptr,
                          fontFile.GetAddressOf()),
                      "CreateInMemoryFontFileReference");
        check_hresult(fontSetBuilder->AddFontFile(fontFile.Get()),
                      "IDWriteFontSetBuilder1::AddFontFile");
        fontFiles_.push_back(std::move(fontFile));
    }
    if (std::find(assetFamiliesFound.begin(), assetFamiliesFound.end(),
                  false) != assetFamiliesFound.end()) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "The font loader did not provide all required families");
    }

    check_hresult(fontSetBuilder->CreateFontSet(fontSet_.GetAddressOf()),
                  "IDWriteFontSetBuilder::CreateFontSet");

    Microsoft::WRL::ComPtr<IDWriteFontCollection1> fontCollection;
    check_hresult(factory_->CreateFontCollectionFromFontSet(
                      fontSet_.Get(), fontCollection.GetAddressOf()),
                  "IDWriteFactory3::CreateFontCollectionFromFontSet");
    check_hresult(fontCollection.As(&fontCollection_),
                  "QueryInterface(IDWriteFontCollection)");

    constexpr std::array<FontFamily, 3> supportedFamilies = {
        FontFamily::Lexend, FontFamily::AtkinsonHyperlegible,
        FontFamily::OpenDyslexic};
    for (const FontFamily family : supportedFamilies) {
        BOOL exists = FALSE;
        UINT32 familyIndex = 0;
        check_hresult(fontCollection_->FindFamilyName(
                          family_name(family), &familyIndex, &exists),
                      "IDWriteFontCollection::FindFamilyName");
        if (!exists) {
            throw ReaderError(
                ReaderErrorCode::FontUnavailable,
                "A required bundled font family is missing from DirectWrite");
        }
    }

    loaderRegistered_ = true;
    registration.transfer_to_owner();
}

InMemoryFontCollection::~InMemoryFontCollection() {
    fontCollection_.Reset();
    fontSet_.Reset();
    fontFiles_.clear();
    if (loaderRegistered_) {
        const HRESULT result =
            factory_->UnregisterFontFileLoader(memoryLoader_.Get());
        if (FAILED(result)) {
            OutputDebugStringA(
                "Dyslexia Reader: failed to unregister in-memory font loader.\n");
        }
    }
    memoryLoader_.Reset();
}

}  // namespace dyslexia_reader::windows
