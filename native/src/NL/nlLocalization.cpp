#include "NL/nlLocalization.h"
#include "port/endian.h"
#include "platform/sdk/net/np_lan.h"  // multiplayer (np/patches/mp)
extern "C" void NpCtlPatchLocalization(nlLocalization* localization) __attribute__((weak));  // W5 CTL (platform/sdk/input)

#include <string.h>

#include "NL/nlFile.h"
#include "NL/nlMemory.h"
#include "NL/nlPrint.h"

#ifdef NP_NATIVE
// a wide string literal cannot initialise an unsigned short array in standard C++
extern const unsigned short LocalizationTableNotFound[] = { 'L', 'o', 'c', 'a', 'l', 'i', 'z', 'a', 't', 'i', 'o', 'n', ' ', 'T', 'a', 'b', 'l', 'e', ' ', 'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd', 0 };
extern const unsigned short MissingLocString[] = { 'm', 'i', 's', 's', 'i', 'n', 'g', ' ', 'l', 'o', 'c', ' ', 's', 't', 'r', 'i', 'n', 'g', 0 };
#else
extern const unsigned short LocalizationTableNotFound[] = L"Localization Table Not Found";
extern const unsigned short MissingLocString[] = L"missing loc string";
#endif

const unsigned long nlLocalization::LanguageId[] = {
    0x7A947B29,
    0xA93C2035,
    0xAAAD26B9,
    0xB482A4B5,
    0xBC0FCCA1,
    0x95F1D726,
    0x5F2F5E69,
    0x30D469C4,
    0x2F242024,
    0x983D29BB,
    0x00012332,
};

char* nlLocalization::LanguageName[] = {
    "English",
    "French",
    "German",
    "Spanish",
    "Italian",
    "Japanese",
    "UKEnglish",
    "NAFrench",
    "NASpanish",
    "Longest",
    "Bob",
};

// The retained thumbprint includes a trailing zero; validation compares four bytes.
const char nlLocalization::Thumbprint[5] = "NLOC";

nlLocalization* g_pLocalization;

static void OnTableLoaded(LOCHeader* pFile, unsigned long fileSize, nlLocalization* pLocalization)
{
    pLocalization->m_pFile = pFile;

    // NP: the callback also runs with a null buffer when the file is missing.
    if (pFile == 0)
    {
        pLocalization->m_LookupTable = 0;
        pLocalization->m_FirstString = 0;
        return;
    }

    // NP: big-endian on disc. First the four u32s behind the thumbprint (the Version check below
    // reads one of them), then the lookup table (hash + offset per string), then every 16-bit
    // character up to the end of the file. A table whose count does not fit the file is refused
    // by clearing Version rather than walked out of bounds.
    if (fileSize >= sizeof(LOCHeader) && memcmp(pFile, nlLocalization::Thumbprint, 4) == 0)
    {
        port_disc32_array((char*)pFile + 4, 4);
        if ((unsigned long long)pFile->StringCount * 8ULL + sizeof(LOCHeader) <= fileSize)
        {
            u32* pLookup = (u32*)(pFile + 1);
            port_disc32_array(pLookup, pFile->StringCount * 2);

            u16* pStrings = (u16*)&pLookup[pFile->StringCount * 2];
            const char* pEnd = (const char*)pFile + fileSize;
            for (u16* w = pStrings; (const char*)(w + 1) <= pEnd; w++)
                *w = port_disc16(w);
        }
        else
        {
            pFile->Version = 0;
        }
    }

    if (memcmp(pFile, nlLocalization::Thumbprint, 4) != 0 || pFile->Version != 1
        || pFile->Language != nlLocalization::LanguageId[pLocalization->m_CurrentLanguage])
    {
        nlFree(pFile);
        pLocalization->m_pFile = 0;
        pLocalization->m_LookupTable = 0;
        pLocalization->m_FirstString = 0;
        return;
    }

    pLocalization->m_LookupTable = (nlLocalization::StringLookup*)(pFile + 1);
    pLocalization->m_FirstString =
        (unsigned short*)(&pLocalization->m_LookupTable[pLocalization->m_pFile->StringCount]);
    NpLanPatchLocalization(pLocalization);  // multiplayer: the old Wi-Fi menu entry is named MULTIPLAYER (np/patches/mp)
    if (NpCtlPatchLocalization) NpCtlPatchLocalization(pLocalization);  // W5 CTL: the prompts name the Vita's buttons (platform/sdk/input/np_prompts.cpp)
}

void nlLocalization::Initialize()
{
    nlLocalization* pLocalization = (nlLocalization*)nlMalloc(sizeof(nlLocalization), 8, false);
    if (pLocalization != 0)
    {
        pLocalization->m_pFile = 0;
        pLocalization->m_LookupTable = 0;
        pLocalization->m_FirstString = 0;
    }
    g_pLocalization = pLocalization;
}

unsigned char nlLocalization::Load(nlLanguage Language, bool ingameloc, void* allocator)
{
    char Filename[64] = { 0 };

    m_LookupTable = 0;
    m_FirstString = 0;
    m_CurrentLanguage = Language;

    if (ingameloc)
    {
        nlSNPrintf(Filename, 64, "art/fe/%s_game.loc", LanguageName[Language]);
    }
    else
    {
        nlSNPrintf(Filename, 64, "art/fe/%s.loc", LanguageName[Language]);
    }

    int result;
    if (allocator != 0)
    {
        result = nlLoadEntireFileAsync(Filename, (LoadAsyncCallback)OnTableLoaded, this, 32, AllocateStart, 0, 0, (MemoryAllocator*)allocator);
    }
    else
    {
        result = nlLoadEntireFileAsync(Filename, (LoadAsyncCallback)OnTableLoaded, this, 32, AllocateStart, 0, 0, 0);
    }

    return result != 0;
}
