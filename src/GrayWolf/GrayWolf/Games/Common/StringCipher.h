#pragma once

// The string cipher of Cold War and Modern Warfare 2019: a zone string's text encrypted as
// [id 0x80-0xBF][b1][n][n - 1 bytes]. The game's DecryptString is, for every cipher the zones use, a
// byte map per position (rol(c ^ k, r)), tabulated from the decrypted exe into the profile's
// strings.cipher.txt. A user has no such table (they are not shipped): one is rebuilt from the
// install's own zones (CipherFit.h, DiskGame.Cipher.cpp) and stands for it.

#include <memory>
#include <string>

namespace StringCipher
{
    struct Table;
    // The table in Dir (a zonekit profile's folder), read once per folder, else the one rebuilt for
    // it (UseRebuilt); nullptr (and why) when there is none
    const Table* Load(const std::string& Dir, std::string& Error);
    // Whether Dir holds its own table (the exe-made strings.cipher.txt, which wins)
    bool HasOwn(const std::string& Dir);
    // A table file (strings.cipher.txt's format), nullptr and why when it does not read
    std::shared_ptr<const Table> ReadFile(const std::string& Path, std::string& Error);
    // The table Load gives for Dir when Dir has none of its own (nullptr: none again)
    void UseRebuilt(const std::string& Dir, std::shared_ptr<const Table> T);
    // A string's text: decrypted when it starts with a cipher the table knows, as it is when it is
    // not encrypted; false when it is encrypted with no cipher known (Text left as it was)
    bool Decrypt(const Table* Cipher, const std::string& Raw, std::string& Text);
}
