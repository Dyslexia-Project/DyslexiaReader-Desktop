#include "dyslexia_reader/windows/platform_profile_client.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

int main() {
    using namespace dyslexia_reader;
    using namespace dyslexia_reader::windows;

    // 1. Endpoint validation: HTTPS & loopback HTTP allowed; plain remote HTTP rejected.
    {
        WindowsPlatformProfileClient httpsClient(
            L"https://platform.dyslexia.example.com");
        WindowsPlatformProfileClient localhostClient(L"http://localhost:8080");
        WindowsPlatformProfileClient loopbackIpClient(L"http://127.0.0.1:8080");
        (void)httpsClient;
        (void)localhostClient;
        (void)loopbackIpClient;

        bool rejectedPlainHttp = false;
        try {
            WindowsPlatformProfileClient insecure(
                L"http://platform.dyslexia.example.com");
        } catch (const ReaderError& err) {
            rejectedPlainHttp = (err.code() == ReaderErrorCode::NetworkFailed);
        }
        CHECK(rejectedPlainHttp);
    }

    // 2. Share code normalization & validation.
    {
        CHECK(WindowsPlatformProfileClient::normalize_share_code(
                  " ab12cd34 \n") == "AB12CD34");

        bool rejectedShort = false;
        try {
            (void)WindowsPlatformProfileClient::normalize_share_code("ab1");
        } catch (const ReaderError& err) {
            rejectedShort = (err.code() == ReaderErrorCode::InvalidProfile);
        }
        CHECK(rejectedShort);

        bool rejectedSymbols = false;
        try {
            (void)WindowsPlatformProfileClient::normalize_share_code("ab12/cd");
        } catch (const ReaderError& err) {
            rejectedSymbols = (err.code() == ReaderErrorCode::InvalidProfile);
        }
        CHECK(rejectedSymbols);
    }

    // 3. Round-trip canonical schema v1 profile serialization & parsing.
    {
        ReadingProfile profile;
        profile.schemaVersion = 1;
        profile.fontFamily = FontFamily::AtkinsonHyperlegible;
        profile.fontSizePt = 22.0F;
        profile.letterSpacingEm = 0.08F;
        profile.wordSpacingEm = 0.25F;
        profile.lineHeight = 1.6F;
        profile.lineWidthCh = 64;
        profile.theme = ThemeId::Sepia;
        profile.aids.bionicReading = true;
        profile.aids.readingRuler = false;
        profile.aids.bimodalHighlighting = true;

        const std::string json =
            WindowsPlatformProfileClient::serialize_profile_json(profile);
        CHECK(json.find("\"schemaVersion\":1") != std::string::npos);
        CHECK(json.find("\"fontFamily\":\"Atkinson Hyperlegible\"") !=
              std::string::npos);
        CHECK(json.find("\"theme\":\"sepia\"") != std::string::npos);
        CHECK(json.find("\"bionicReading\":true") != std::string::npos);

        const ReadingProfile parsed =
            WindowsPlatformProfileClient::parse_profile_json(json);
        CHECK(parsed.schemaVersion == 1);
        CHECK(parsed.fontFamily == FontFamily::AtkinsonHyperlegible);
        CHECK(parsed.fontSizePt == 22.0F);
        CHECK(parsed.lineWidthCh == 64);
        CHECK(parsed.theme == ThemeId::Sepia);
        CHECK(parsed.aids.bionicReading);
        CHECK(!parsed.aids.readingRuler);
        CHECK(parsed.aids.bimodalHighlighting);
    }

    // 4. Reject out-of-bounds profiles cleanly.
    {
        const std::string outOfBounds = R"({
            "schemaVersion": 1,
            "fontFamily": "Lexend",
            "fontSizePt": 99,
            "letterSpacingEm": 0.05,
            "wordSpacingEm": 0.16,
            "lineHeight": 1.6,
            "lineWidthCh": 65,
            "theme": "warm-cream",
            "aids": { "bionicReading": false, "readingRuler": true, "bimodalHighlighting": false }
        })";
        bool rejectedBounds = false;
        try {
            (void)WindowsPlatformProfileClient::parse_profile_json(outOfBounds);
        } catch (const ReaderError& err) {
            rejectedBounds = (err.code() == ReaderErrorCode::InvalidProfile);
        }
        CHECK(rejectedBounds);
    }

    // 5. Parse share response code from Platform POST /api/v1/profiles/share.
    {
        CHECK(WindowsPlatformProfileClient::parse_share_response_code(
                  R"({"code":"zx98qp12"})") == "ZX98QP12");
    }

    std::cout << "platform_profile_smoke: ALL CHECKS PASSED\n";
    return 0;
}
