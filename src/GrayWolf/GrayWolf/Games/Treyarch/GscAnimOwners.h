#pragma once

// Anim owners (AnimOwners.h) from a zone's GSC scripts, animtrees and weapons (World at War, Black
// Ops, Black Ops II; CoD4's are the same files): AI types (aitype/<n>.gsc) and their characters,
// player types (mptype), Black Ops' create-a-class players (mpbody, mphead, the factions of the
// teamset a level starts), Black Ops II's compiled scripts (GSCOBJ: includes, imports, animtrees,
// string literals; the mpbody class files, a zombie level's player characters), animtrees (.atr)
// and the scripts' %anims, and weapons as a zone's weapon assets give them (WeaponDocument).
// A game gives the documents of a zone and of what loads with it (TreyarchGscOwners.h).

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace AnimOwners { struct Book; }

namespace GscAnimOwners
{
    struct Rules
    {
        std::string AiTree = "generic_human";       // an AI type's tree when self.animTree is ""
        std::string PlayerTree = "multiplayer";     // a player type's (the engine's player tree)
        bool PlayerTypes = true;                    // the mptype files are players (Black Ops': bodies no zone holds)
        std::string ZombiePlayerTree = "zombie_player";   // a zombie level's player characters' tree
    };

    // an AI type or a player type: the tree its anims are of, its characters (one picked at spawn)
    struct Type
    {
        std::string Kind, Name, Tree;               // Kind: "aitype", "mptype"
        std::vector<std::string> Characters;
    };
    // a character's body and head: a model, or an xmodelalias (a list, one picked at spawn)
    struct Character
    {
        std::string Body, BodyAlias, Head, HeadAlias;
    };
    // a player the engine animates with a player tree (Tree, or Tree_<variant>): its models, or a character's
    struct Player
    {
        std::string Id, Tree, Character;
        std::vector<std::string> Models;
        std::string Source;
    };
    // a weapon variant (its gun model and hands) or an attachment unique (Unique: the gun is the
    // base weapon's), and the xanims it names
    struct Weapon
    {
        std::string Gun, Hands;
        std::vector<std::string> Anims;
        bool Unique = false;
    };
    // what a zone's files say (names lower case)
    struct Facts
    {
        std::map<std::string, std::set<std::string>> Trees;   // tree -> its anims
        std::vector<Type> Types;
        std::map<std::string, Character> Characters;
        std::map<std::string, std::vector<std::string>> Aliases;
        std::vector<Player> Players;
        std::map<std::string, Weapon> Weapons;
        // the players' first-person arms (setviewmodel), a weapon's hands when it names none (Black Ops II)
        std::vector<std::string> ViewHands;
        // Black Ops' create-a-class assets (mpbody, mphead): asset -> faction -> model; a body's head
        std::map<std::string, std::map<std::string, std::string>> CacBodies, CacHeads;
        std::map<std::string, std::string> DefaultHead;
        std::map<std::string, std::vector<std::string>> TeamsetFactions;   // teamset -> its factions (allies, axis)
        std::vector<std::string> Teamsets;                                 // the teamsets a level starts
    };

    // How Own ranks and widens owners (World at War: none of these)
    struct Options
    {
        // an anim of no tree named after an AI type's body (german_shepherd_run) is that type's, when
        // the type's tree lists no anim in any source (the multiplayer dog's)
        bool ByModelPrefix = false;
        // of an anim's AI types, those sharing a word with it that not all of them share first
        // (ai_zombie_quad_crawl: zombie_quad before zombie_ger_zombie)
        bool ByNameTokens = false;
        size_t WeaponOwners = 4;   // weapons naming an anim kept (distinct gun models, nearest name first)
    };

    // the document names read (IDiskGame::AnimOwnerDocuments): gsc, csc, atr and weapon documents
    std::vector<std::string> Documents();
    // A weapon's document for Parse: "weapons/<name>.weapon" (attachment uniques
    // "attachmentunique/<name>.weapon"), lines "gun\t<model>", "hands\t<model>", "anim\t<xanim>"
    std::pair<std::string, std::string> WeaponDocument(const std::string& Name, bool Unique, const std::string& Gun,
                                                       const std::string& Hands, const std::vector<std::string>& Anims);
    // (name, text) documents -> facts; a compiled script (Black Ops II's GSCOBJ) is read from its bytes
    Facts Parse(const std::vector<std::pair<std::string, std::string>>& Docs, const Rules& R = {});
    // Anims' owners into Out. Sources: the zone's own facts first, then what loads with it, an
    // earlier one winning. An anim of tree T: the types of T of the first source holding any, then
    // T's players; an anim weapons name: those weapons.
    void Own(const std::vector<const Facts*>& Sources, const std::vector<std::string>& Anims, AnimOwners::Book& Out,
             const Options& O = {});
}
