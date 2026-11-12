#pragma once

// The string cipher's table (StringCipher.h) fitted from a game's encrypted zone strings alone, no
// exe: byte i of [id][b1][n][text] is rol(c ^ k, r), the cipher picked by (id, b1). Facts used: b1
// is bits 32-39 of FNV-1a 64 of the lower-case text (not Cold War), neighbouring b1 share a cipher
// (groups found from the data), a cipher keeps one k or follows a k -> r map learned from the
// checked cells. Method: graywolf-tools/scripts/cipher_from_zones.

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace CipherFit
{
    // Whether a zone string is one the cipher encrypts: [id 0x80-0xBF][b1][n][n - 1 bytes], n >= 2
    bool Encrypted(const std::string& S);

    struct Options
    {
        bool HashChecked = true;                 // b1 is the text's hash
        // plain names of the kind the strings are (the name databases' bones and strings): the
        // first character model with the hand-written words, then blended with the checked strings
        std::vector<std::string> Corpus;
        unsigned Threads = 0;                    // 0: the machine's, at most 8
        std::function<void(const std::string&)> Log;
        std::function<void(size_t Done, size_t Count)> Step;   // stages done of their count
        const std::atomic<bool>* Cancel = nullptr;
    };

    struct Result
    {
        std::string Table;                       // a strings.cipher.txt's text
        size_t Strings = 0, Checked = 0, Slots = 0;
        int Groups = 0;                          // the b1 groups found
    };

    // Fits the table of these strings (any order, repeats and plain ones ignored); false when
    // cancelled or when there is nothing to fit
    bool Fit(const std::vector<std::string>& Strings, const Options& O, Result& Out);
}
