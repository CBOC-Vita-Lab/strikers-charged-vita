// np_text.h (1.3): the language choice, the lookup, the sentence-by-sentence translation and the UTF-8 -> UTF-16 step of the game's popups.
//   c++ -std=c++17 -Wall -Wextra -I<native> native/platform/sdk/tests/test_text.cpp -o /tmp/test_text && /tmp/test_text   (tests/test_text_table.py runs it)
#include "platform/sdk/include/np/np_text.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string Sent(int lang, const char* in, size_t size = 1024) {
    char out[1024];
    NpText::Sentences(lang, in, out, size);
    return out;
}

int main() {
    using namespace NpText;
    CHECK(ParseLanguage("es") == kSpanish && ParseLanguage("ES") == kSpanish && ParseLanguage("Spanish") == kSpanish && ParseLanguage("sp") == kSpanish);
    CHECK(ParseLanguage("fr") == kFrench && ParseLanguage("French") == kFrench && ParseLanguage("en") == kEnglish);
    CHECK(ParseLanguage("de") == -1 && ParseLanguage("") == -1 && ParseLanguage(nullptr) == -1);

    // the lookup: a known text, an unknown one (itself), English (itself)
    CHECK(std::strcmp(In(kSpanish, "LANGUAGE"), "IDIOMA") == 0);
    CHECK(std::strcmp(In(kFrench, "LANGUAGE"), "LANGUE") == 0);
    const char* unknown = "NOT A TEXT OF THE TABLE";
    CHECK(In(kSpanish, unknown) == unknown && In(kEnglish, "LANGUAGE") != nullptr && std::strcmp(In(kEnglish, "LANGUAGE"), "LANGUAGE") == 0);
    CHECK(In(kSpanish, nullptr) == nullptr && In(7, "LANGUAGE") != nullptr && std::strcmp(In(7, "LANGUAGE"), "LANGUAGE") == 0);

    // sentences: a reason the table has followed by one it has, one it does not have, the "..." of a status line, English untouched
    CHECK(Sent(kSpanish, "The other player left the match. The match goes on.") == "El otro jugador abandonó el partido. El partido continúa.");
    CHECK(Sent(kSpanish, "Something new happened. The match goes on.") == "Something new happened. El partido continúa.");
    CHECK(Sent(kFrench, "Connecting to the service... Please wait. Press Circle to cancel.") ==
          "Connexion au service... Veuillez patienter. Appuyez sur Rond pour annuler.");
    CHECK(Sent(kEnglish, "The other player left the match. The match goes on.") == "The other player left the match. The match goes on.");
    CHECK(Sent(kSpanish, "  Leading spaces stay.  The match goes on.") == "  Leading spaces stay.  El partido continúa.");
    CHECK(Sent(kSpanish, "No end mark") == "No end mark" && Sent(kSpanish, "") == "");
    CHECK(Sent(kSpanish, "v1.3 is a version. The match goes on.") == "v1.3 is a version. El partido continúa.");   // (a '.' inside a word is no end)
    // a short buffer: cut, never overrun, always terminated
    CHECK(Sent(kSpanish, "The other player left the match. The match goes on.", 8) == "El otro");
    {
        char tiny[1] = {'x'};
        Sentences(kSpanish, "The match goes on.", tiny, 1);
        CHECK(tiny[0] == 0);
    }

    // UTF-8 to the game's UTF-16
    unsigned short w[16];
    CHECK(ToUtf16("Año ¿sí?", w, 15) == 8 && w[1] == 0xF1 && w[4] == 0xBF && w[6] == 0xED && w[8] == 0);
    CHECK(ToUtf16("a\nb\tc", w, 15) == 5 && w[1] == '\n' && w[3] == '?');
    CHECK(ToUtf16("\xF0\x9F\x98\x80" "x", w, 15) >= 2 && w[0] == '?');   // (a 4-byte sequence is not drawn)
    CHECK(ToUtf16("abcdef", w, 3) == 3 && w[3] == 0);
    CHECK(ToUtf16(nullptr, w, 3) == 0 && w[0] == 0);

    // every row of the table: Spanish and French present
    int count = 0;
    const Entry* t = Table(&count);
    CHECK(count > 300);
    for (int i = 0; i < count; ++i) CHECK(t[i].en && *t[i].en && t[i].es && *t[i].es && t[i].fr && *t[i].fr);

    // texts with a placeholder: the formatted English is matched against the template row, the pieces go into the translated one
    CHECK(Sent(kSpanish, "The online service refused the request (status 503).") == "El servicio en línea rechazó la solicitud (estado 503).");
    CHECK(Sent(kFrench, "The online service refused the request (room_full).") == "Le service en ligne a refusé la demande (room_full).");
    CHECK(Sent(kSpanish, "Different game version: please update the game to 1.3.1.") == "Versión diferente: actualiza el juego a la versión 1.3.1.");
    CHECK(Sent(kSpanish, "Different game version: please update the game.") == "Versión diferente: actualiza el juego.");
    CHECK(Sent(kSpanish, "The relay could not be set up (no answer).") == "No se pudo configurar el relé (sin respuesta).");
    CHECK(Sent(kFrench, "The relay could not be set up (turn_upstream).") == "Le relais n'a pas pu être configuré (turn_upstream).");
    CHECK(Sent(kSpanish, "The relay server refused to set up the connection (error 437).") == "El servidor de relé rechazó crear la conexión (error 437).");
    CHECK(Sent(kFrench, "The relay server refused to set up the connection (error -1).") == "Le serveur relais a refusé d'établir la connexion (erreur -1).");
    CHECK(Sent(kSpanish, "Using the relay (no direct path).") == "Usando el relé (sin ruta directa).");
    CHECK(Sent(kFrench, "Using the relay (the other player uses the relay).") == "Utilisation du relais (l'autre joueur utilise le relais).");
    CHECK(Sent(kSpanish, "Using the relay (advised by the server: busy).") == "Usando el relé (advised by the server: busy).");   // (a piece of the service's own text stays)
    CHECK(Sent(kSpanish, "A newer version of the game is needed to play online (1.4).") == "Se necesita una versión más reciente del juego para jugar en línea (1.4).");
    CHECK(Sent(kSpanish, "The relay server refused to set up the connection (error x).") == "The relay server refused to set up the connection (error x).");   // (%d wants digits)
    CHECK(Sent(kSpanish, "The online service refused the request (status 503). The match goes on.") ==
          "El servicio en línea rechazó la solicitud (estado 503). El partido continúa.");   // per sentence
    CHECK(Sent(kEnglish, "The relay could not be set up (no answer).") == "The relay could not be set up (no answer).");
    CHECK(Sent(kSpanish, "The online service refused the request ().") == "The online service refused the request ().");   // (a %s takes something)
    // (words3) the fatal screen's lines of the disc reader: a template with several numbers, and with a hex address as a %s
    CHECK(Sent(kSpanish, "Read 10 of 20 bytes at 30; disc expects 40 bytes.") == "Se leyeron 10 de 20 bytes en la posición 30; el disco espera 40 bytes.");
    CHECK(Sent(kFrench, "Read 10 of 20 bytes at 30; disc expects 40 bytes.") == "Lu 10 sur 20 octets à la position 30; le disque attend 40 octets.");
    CHECK(Sent(kFrench, "The game requested an unknown disc address (0x1f00, 64 bytes).") == "Le jeu a demandé une adresse de disque inconnue (0x1f00, 64 octets).");
    {
        char out[16];
        CHECK(Template(kSpanish, "Using the relay (no direct path).", out, sizeof out) && std::strlen(out) == 15 && out[15] == 0);   // cut, terminated
        CHECK(!Template(kSpanish, "nothing like it", out, sizeof out) && !Template(kEnglish, "Using the relay (x).", out, sizeof out));
    }
    // the only conversions the table may hold: %s %d %u %lu (%zu and %lld are skipped by the Vita's C library)
    for (int i = 0; i < count; ++i)
        for (const char* text : {t[i].en, t[i].es, t[i].fr})
            for (const char* p = std::strchr(text, '%'); p; p = std::strchr(p + 1, '%')) {
                const char c = p[1];
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                CHECK(!letter || c == 's' || c == 'd' || c == 'u' || (c == 'l' && p[2] == 'u'));
            }

    std::printf("test_text: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
