#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct SeAnimClip;
struct XAnimBinRig;

// Reads the BO3 Mod Tools' `.xanim_bin` format - the compiled form of a `.XANIM_EXPORT` text file,
// produced by `export2bin.exe`, reverse-engineered from that tool.
//
// NOT the same format as the retail game's own compiled `XAnimParts` (the struct actually stored
// in retail `.ff` fast files). This reader targets the ~980 animations the Mod Tools ship on disk
// under `xanim_export/`.

// `valueType` (XAnimBinValueType) and the record tags, shared with the writers: XBin.h (Core).
#include "XBin.h"

// One resolved field record from the tagged-record stream. Numeric sub-fields are kept as raw
// wire bytes in `Raw` (in field order, no re-interpretation - e.g. Raw32x3's 12 bytes may be
// uint32 or float depending on the keyword, the writer never distinguishes at this layer) so
// Layer 1 never has to guess int-vs-float; a consumer memcpy's out what each `Keyword` implies.
// String-bearing types (0/0xe/0xf/0x10/0x11) get their decoded strings in `Strings` instead, in
// field order, with `Raw` holding just that type's leading non-string sub-field(s) if any.
struct XAnimBinRecord
{
    std::string Keyword;              // Resolved keyword text (e.g. "BONE", "FRAME", "NUMFRAMES").
    XAnimBinValueType Type = XAnimBinValueType::Marker;
    uint32_t TableIndex = 0;          // Position in the 60-entry keyword table - lets a consumer
                                       // tell apart the 3 BONE overloads etc. (same keyword text,
                                       // different `Type` - see the doc's own note on this).
    std::vector<uint8_t> Raw;         // Fixed-shape numeric payload, wire order, wire bytes.
    std::vector<std::string> Strings; // String sub-fields, in field order (String/UInt16String/
                                       // UInt16String3/UInt32String/UInt32x2String only).
    std::vector<std::pair<float, float>> UVs; // UVArray only.
};

// Layer 1 (mechanical, from the decompiled writer): decompress
// the LZ4 container and walk the payload as a sequence of [2-byte CRC tag][type-coded payload]
// records, resolving every tag against the precomputed 60-keyword table. Returns false on a bad
// magic, a failed LZ4 decompress, or any record whose tag doesn't match a known keyword (a real
// desync, not a soft stop - this format has no resync point once a record's length is misjudged).
bool ReadXAnimBinRecords(const std::string& Path, std::vector<XAnimBinRecord>& Out);

// The other way: the records encoded as export2bin does (XBin.h) into a .xanim_bin / .xmodel_bin.
// What ReadXAnimBinRecords read gives the same payload back (xanimbin-rewrite checks it on the
// Mod Tools' files). False when the file cannot be written.
bool WriteXAnimBinRecords(const std::vector<XAnimBinRecord>& Records, const std::string& Path);

// The quaternion (x, y, z, w) of a rotation given by its axes in model space: an XANIM_BIN's or
// XMODEL_BIN's X, Y and Z lines, the rotation matrix's columns
void XAnimBinAxesToQuat(const double X[3], const double Y[3], const double Z[3], double Q[4]);

// Layer 2: assembles the tagged-record stream into a SeAnimClip - PART declarations become the
// bone list, FRAME/PART(select)/OFFSET/X/Y/Z become per-frame per-bone pos+rot keys (X/Y/Z are 3
// rows of a rotation matrix, converted to a quaternion here - see the .cpp for the exact
// convention and its caveats), NUMKEYS + FRAME(0x10)'s uint32-frame+string pairs become
// `Out.Notifications`. Not verified against a decompiled parser or a text source: the grammar is
// inferred from XANIM_EXPORT convention and checked only via structural invariants (declared
// NUMPARTS/NUMFRAMES/NUMKEYS must match what is assembled, else the read fails).
// NB: the keys are the file's values as they are, and an XANIM_BIN's are model-space (each part's
// pose in the model, its parents' moves included; X/Y/Z the rotation's axes, the matrix's columns,
// so the rows reading below gives the inverse rotation), not parent-relative: local keys need the
// skeleton (XAnimBinExport.h, ReadXAnimBinRig; the xanimbin-compare command converts them).
bool ReadXAnimBinClip(const std::string& Path, SeAnimClip& Out);

// The skeleton an XANIM_BIN is written for (XAnimBinExport.h): the model the anim's GDT entry
// names, a .xmodel_bin (its token stream: BONE index parent name, then each bone's OFFSET and X/Y/Z
// axes) or a .xmodel_export (text). False, with Error, when the file holds no bone or a parent
// after its child.
bool ReadXAnimBinRig(const std::string& Path, XAnimBinRig& Out, std::string& Error);
