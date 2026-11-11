#include <iostream>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>


int main(int argc, char* argv[]) {
    std::cout << "=== Dyslexia Reader Desktop — Snipping OCR Test ===" << std::endl;

    // Calea către imagine (dacă nu este dată ca argument, căutăm test.png în folderul părinte)
    const char* imagePath = (argc > 1) ? argv[1] : "../test.png";

    // 1. Inițializăm motorul Tesseract OCR
    tesseract::TessBaseAPI tess;
    if (tess.Init(nullptr, "eng")) {
        std::cerr << "[EROARE] Nu s-a putut inițializa motorul Tesseract OCR." << std::endl;
        return 1;
    }

    // 2. Încărcăm imaginea folosind cititorul intern Pix din Tesseract
    Pix* image = pixRead(imagePath);
    if (!image) {
        std::cerr << "[EROARE] Nu s-a putut deschide imaginea: " << imagePath << std::endl;
        std::cerr << "Sfat: Creează o captură de ecran cu 'Cmd + Shift + 4' și salveaz-o ca 'test.png'!" << std::endl;
        tess.End();
        return 1;
    }

    // 3. Procesăm imaginea cu Tesseract
    tess.SetImage(image);
    char* outText = tess.GetUTF8Text();

    std::cout << "\n---------------- TEXT DETECTAT ----------------" << std::endl;
    std::cout << outText << std::endl;
    std::cout << "-----------------------------------------------" << std::endl;
    std::cout << "[SUCCESS] OCR finalizat cu succes!" << std::endl;

    // Eliberăm resursele
    delete[] outText;
    pixDestroy(&image);
    tess.End();

    return 0;
}