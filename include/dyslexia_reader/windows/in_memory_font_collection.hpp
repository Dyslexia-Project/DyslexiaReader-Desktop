#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <dwrite_3.h>
#include <wrl/client.h>

namespace dyslexia_reader::windows {

class InMemoryFontCollection final {
public:
    InMemoryFontCollection(IDWriteFactory* factory, IFontLoader& fontLoader);
    ~InMemoryFontCollection();

    InMemoryFontCollection(const InMemoryFontCollection&) = delete;
    InMemoryFontCollection& operator=(const InMemoryFontCollection&) = delete;

    IDWriteFontCollection* get() const noexcept {
        return fontCollection_.Get();
    }

private:
    Microsoft::WRL::ComPtr<IDWriteFactory5> factory_;
    Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> memoryLoader_;
    Microsoft::WRL::ComPtr<IDWriteFontSet> fontSet_;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection_;
    std::vector<Microsoft::WRL::ComPtr<IDWriteFontFile>> fontFiles_;
    bool loaderRegistered_ = false;
};

}  // namespace dyslexia_reader::windows
