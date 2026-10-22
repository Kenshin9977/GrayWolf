#include "stdafx.h"

#include "Bo3SoundAliases.h"

#include "Games/Treyarch/TreyarchSoundNames.h"

#include <algorithm>
#include <filesystem>

// Ported from HydraX's BlackOps3 Sound pool (github.com/Scobalula/HydraX, Sound.cs, GPL-3.0): the
// SoundAliasEntry layout, its flag bits and the enum tables, checked against the mod tools'
// share/raw/sound/globals (pan, volume_group, duck_group, curve, futz).

using SoundAliasCsv::Col;

namespace
{
    // SndBank (0x220b0): aliases 0x20/0x28, ducks 0x48/0x50 (0x88 each, name char[64], hash 0x40)
    constexpr int64_t AliasCountAt = 0x20, AliasesAt = 0x28, DuckCountAt = 0x48, DucksAt = 0x50, DuckSize = 0x88;
    // SoundAlias (0x28): name 0, hash 8, entries 0x10, count 0x18
    constexpr int64_t AliasSize = 0x28;
    // SoundAliasEntry (0xd8): what each offset below holds is in the fields' comments
    constexpr int64_t EntrySize = 0xd8;

    constexpr const char* VolumeGroups[] = {
        "grp_air", "grp_alerts", "grp_ambience", "grp_announcer", "grp_bink", "grp_destructible", "grp_explosion",
        "grp_foley", "grp_hdrfx", "grp_igc", "grp_impacts", "grp_lfe", "grp_master", "grp_menu", "grp_mp_game",
        "grp_music", "grp_physics", "grp_player_foley", "grp_player_impacts", "grp_reference", "grp_scripted_moment",
        "grp_set_piece", "grp_vehicle", "grp_voice", "grp_weapon", "grp_weapon_3p", "grp_weapon_decay_3p", "grp_whizby",
        "grp_wpn_lfe",
    };
    constexpr const char* Pans[] = {
        "default", "music", "wpn_all", "wpn_fnt", "wpn_rear", "wpn_left", "wpn_right", "music_all", "fly_foot_all",
        "front", "back", "front_mostly", "back_mostly", "all", "center", "center_mostly", "front_and_center", "lfe",
        "quad", "front_mostly_some_center", "front_halfback", "halffront_back", "test", "brass_right", "brass_left",
        "veh_back", "tst_left", "tst_center", "tst_right", "tst_surround_left", "tst_surround_right", "tst_lfe",
        "tst_back_left", "tst_back_right", "pip", "movie_vo",
    };
    constexpr const char* FluxTypes[] = {
        "", "left_player", "center_player", "right_player", "random_player", "left_shot", "center_shot", "right_shot",
        "random_direction",
    };
    constexpr const char* Curves[] = {
        "default", "defaultmin", "allon", "alloff", "rcurve0", "rcurve1", "rcurve2", "rcurve3", "rcurve4", "rcurve5",
        "steep", "sindelay", "cosdelay", "sin", "cos", "rev60", "rev65",
    };
    constexpr const char* DuckGroups[] = {
        "snp_alerts_gameplay", "snp_ambience", "snp_claw", "snp_destructible", "snp_dying", "snp_dying_ice", "snp_evt_2d",
        "snp_explosion", "snp_foley", "snp_grenade", "snp_hdrfx", "snp_igc", "snp_impacts", "snp_menu", "snp_movie",
        "snp_music", "snp_never_duck", "snp_player_dead", "snp_player_impacts", "snp_scripted_moment", "snp_set_piece",
        "snp_special", "snp_vehicle", "snp_vehicle_interior", "snp_voice", "snp_voice_announcer", "snp_weapon_decay_1p",
        "snp_whizby", "snp_wpn_1p", "snp_wpn_1p_act", "snp_wpn_1p_ads", "snp_wpn_1p_shot", "snp_wpn_3p",
        "snp_wpn_3p_decay", "snp_wpn_turret", "snp_x2", "snp_x3", "snp_distant_explosions",
    };
    constexpr const char* LimitTypes[] = { "none", "oldest", "reject", "priority" };
    constexpr const char* StorageTypes[] = { "unknown", "loaded", "streamed", "primed" };
    // the mod tools' templates write them in lower case (bus_fx)
    constexpr const char* Buses[] = { "bus_fx", "bus_voice", "bus_pfutz", "bus_hdrfx", "bus_ui", "bus_music", "bus_movie", "bus_reference" };

    // Duck and context names, futz patches (share/raw/sound/globals/futz.csv): the hashes HydraX knows
    constexpr const char* KnownNames[] = {
        "aquifer_cockpit", "active", "battle", "silent", "slomo", "boot", "wet", "foley", "normal", "igc",
        "healthstate", "human", "cyber", "infected", "on", "laststand", "mature", "explicit", "safe", "movement",
        "loud", "quiet", "perspective", "player", "npc", "plr_body", "reaper", "prophet", "nomad", "outrider",
        "seraph", "ruin", "assassin", "pyro", "grenadier", "plr_gender", "male", "female", "plr_impact", "veh",
        "pwr_armor", "plr_stance", "stand", "crouch", "prone", "plr_vehicle", "driver", "ringoff_plr", "indoor",
        "outdoor", "underwater", "train", "country", "city", "tunnel", "vehicle", "interior", "exterior", "water",
        "under", "over", "amb_battle_ducker", "amb_battle_ducker_half", "amb_lightning_indoor", "amb_lightning_sewer",
        "cmn_3p_weapon_occ", "cmn_aar_screen", "cmn_bleedout", "cmn_bo3_load", "cmn_cc_securitybreach",
        "cmn_cc_securitybreach_trans", "cmn_cc_tacmenu", "cmn_dead_turret_exp", "cmn_death_coop", "cmn_death_plr",
        "cmn_death_solo", "cmn_dni_interrupt", "cmn_duck_all", "cmn_duck_all_but_movie", "cmn_duck_music",
        "cmn_duck_music_dist", "cmn_duck_underscore", "cmn_duck_underscore_and_round", "cmn_health_laststand",
        "cmn_health_low", "cmn_health_resurrect", "cmn_igc_amb_silent", "cmn_igc_bg_lower", "cmn_igc_foley_lower",
        "cmn_igc_skip", "cmn_jump_land_plr", "cmn_kill_foley", "cmn_level_fadeout", "cmn_level_fade_immediate",
        "cmn_level_start", "cmn_melee_pain", "cmn_music_quiet", "cmn_no_vo", "cmn_override_duck", "cmn_pain_plr",
        "cmn_qtank_railgun_shot", "cmn_raps_spawn", "cmn_robot_behind", "cmn_shock_tinitus", "cmn_sniper_fire_3rd",
        "cmn_swimming", "cmn_time_freeze", "cmn_ui_tac_mode", "cmn_wallrun", "cod_ads", "cod_alloff", "cod_allon",
        "cod_doa_fps", "cod_fadein", "cod_hipfire", "cod_hold_breath", "cod_matureduck", "cod_menu",
        "cod_mpl_combat_awareness", "cod_test_alias", "cod_test_env", "cod_xcam", "cp_aquifer_breach",
        "cp_aquifer_breach_slowmo", "cp_aquifer_drown_evt", "cp_aquifer_int", "cp_aquifer_int_deep",
        "cp_aquifer_outro", "cp_aquifer_pip_HeroLocation", "cp_aquifer_script_jet", "cp_aquifer_underwater",
        "cp_aquifer_veh_dogfight", "cp_aquifer_veh_exp_hit", "cp_aquifer_veh_int", "cp_barge_slowtime",
        "cp_biodome2_slide_prj", "cp_biodomes_2_end_vehicle", "cp_biodomes_dive_duckambience",
        "cp_biodomes_supertree_collapse", "cp_biodomes_vtol_esc", "cp_biodome_acc_drive_igc",
        "cp_biodome_acc_drive_igc_2", "cp_biodome_supertree_vtol", "cp_blackstation_boatride",
        "cp_blackstation_boatride_getoff", "cp_blackstation_boatride_geton", "cp_blackstation_data_recorder",
        "cp_blackstation_debris", "cp_blackstation_debris_small", "cp_blackstation_intro_veh",
        "cp_blackstation_intro_veh_2", "cp_blackstation_outro", "cp_blackstation_scripted_wind",
        "cp_blackstation_thunder", "cp_blackstation_warlord_igc", "cp_cybercore_activate", "cp_cybercore_ready",
        "cp_cybercore_unstoppable", "cp_dialog", "cp_infection_flyaway", "cp_infection_hideout_amb",
        "cp_infection_interface", "cp_infection_intro", "cp_infection_intro_2", "cp_infection_intro_imp",
        "cp_infection_labdeath", "cp_infection_qt_birth", "cp_infection_testlab_transition", "cp_life_vinetrans",
        "cp_lotus_delusion_overlay", "cp_lotus_hospital_fade", "cp_lotus_vtol_bridge", "cp_lotus_vtol_hallway",
        "cp_mi_sing_vengeance_slowmo", "cp_newworld_pipes", "cp_prologue_apc_door_close", "cp_prologue_apc_duck_explo",
        "cp_prologue_duck_apc_lps", "cp_prologue_exit_apc", "cp_prologue_outro_rippedapart", "cp_prologue_outro_runup",
        "cp_prologue_vtolflyby", "cp_ramses_celing", "cp_ramses_demostreet_1", "cp_ramses_demostreet_2",
        "cp_ramses_demostreet_3", "cp_ramses_intro_igc", "cp_ramses_int_dead", "cp_ramses_int_veh",
        "cp_ramses_jeep_drive", "cp_ramses_nasser_igc", "cp_ramses_outro", "cp_ramses_plaza_battle",
        "cp_ramses_preplaza", "cp_ramses_pre_vtol", "cp_ramses_qt_vtol", "cp_ramses_qt_wallcrash",
        "cp_ramses_quad_music", "cp_ramses_raps_intro", "cp_ramses_streetcollapse", "cp_ramses_theater_explo",
        "cp_ramses_trans", "cp_ramses_vtol_fall", "cp_ramses_vtol_impact", "cp_ramses_vtol_walk",
        "cp_ramses_wall_3p_gunfire", "cp_safehouse_exit", "cp_sgen_base_explo", "cp_sgen_flooding", "cp_sgen_flyover",
        "cp_sgen_ghost_igc", "cp_sgen_steam_duck", "cp_sgen_twinrevenge_igc", "cp_sgen_uw_boulder", "cp_sgen_wave",
        "cp_sh_cairo_foley_low", "cp_vengeance_cafe", "cp_vengeance_int", "cp_vengeance_int_deep", "cp_warlord_melee",
        "cp_zmb_box_3d", "cp_zmb_ending", "cp_zmb_thelooper", "cp_zmb_timefreeze", "cp_zmb_voice",
        "cp_zurich_duckrabbit", "cp_zurich_end_duckexplo", "cp_zurich_movie", "cp_zurich_movie_long",
        "cp_zurich_train", "default", "dummy", "exp_barrel", "exp_grenade", "exp_medium", "exp_mortar",
        "exp_quad_rocket", "exp_rocket_close", "exp_rocket_quad", "exp_small", "exp_vehicle", "exp_vehicle_close",
        "mpl_announcer", "mpl_death", "mpl_duck_announcer", "mpl_duck_holoscreen", "mpl_endmatch", "mpl_final_killcam",
        "mpl_final_killcam_slowdown", "mpl_hellstorm", "mpl_postmatch", "mpl_post_match", "mpl_prematch",
        "mpl_speedboost_run", "prj_impact", "prj_impact_plr", "prj_whizby", "wpn_cmn_burst_3p", "wpn_cmn_shot_3p",
        "wpn_cmn_shot_plr", "wpn_cmn_suppressed_plr", "wpn_hunter_missile", "wpn_jet_rocket_plr",
        "wpn_melee_knife_plr", "wpn_rpg_plr", "wpn_shotgun_sci", "wpn_shoulder_shot_npc", "wpn_sniper_shot_plr",
        "wpn_turret_npc", "wpn_turret_plr", "zmb_apothifight_beam", "zmb_beastmode_enter", "zmb_bgb_fearinheadlights",
        "zmb_bgb_killingtime", "zmb_castle_bossbattle", "zmb_castle_bossbattle_event", "zmb_castle_duck_evt_3d",
        "zmb_castle_outro", "zmb_castle_timetravel", "zmb_cmn_mk3_orb", "zmb_cosmo_intro", "zmb_cp_song_suppress",
        "zmb_derriese_start", "zmb_dialog", "zmb_dialog_2d", "zmb_dialog_monty", "zmb_duck_close_vehicles",
        "zmb_duck_music_3d", "zmb_easter_egg_song", "zmb_game_over", "zmb_game_start", "zmb_game_start_nofade",
        "zmb_hd_game_start_nofade", "zmb_health_low", "zmb_island_dopple_scream", "zmb_island_duck_bg_music",
        "zmb_island_hallucinate", "zmb_island_inside_thrasher", "zmb_island_swimming", "zmb_island_takeo",
        "zmb_island_trial", "zmb_laststand", "zmb_margwa_smash", "zmb_moon_gasmask", "zmb_moon_space",
        "zmb_radio_duck", "zmb_sophia", "zmb_stalingrad_pa_destruct", "zmb_stal_boss_fight", "zmb_stal_dragon_fight",
        "zmb_stal_outro", "zmb_stal_tbc", "zmb_temple_meteor", "zmb_temple_radio", "zmb_umonkey", "zmb_zhd_laststand",
        "zmb_zod_apothibattle_duck", "zmb_zod_apothigod", "zmb_zod_beastmode", "zmb_zod_cursed",
        "zmb_zod_duck_octobomb_lp", "zmb_zod_endigc", "zmb_zod_scream", "zmb_zod_shadbattle_duck", "zmb_zod_sword",
        "zmb_zod_sword_powerup", "zmb_zod_teleport", "zmb_zod_totem_charge",
        "bfutz", "defaultbkp", "dlc_res_1", "dlc_res_2", "dlc_res_3", "dlc_res_4", "dlc_res_5", "dlc_res_6", "dlc_res_7",
        "dlc_res_8", "good_1", "jet_wing_helmet", "jet_wing_helmet_flying", "mpl_agr_pov", "mpl_chopper_pov",
        "mpl_quad_pov", "mpl_reaper_pov", "no_gfutz", "spl_asd_pov", "spl_bigdog_pov", "spl_heli_future", "spl_quad_pov",
        "spl_spiderbot_pov", "spl_spymic", "spl_tow_missile", "spl_turret", "spl_war_command", "stereo", "test_1",
        "test_2", "tueyeckert",
    };

    bool Bit(uint32_t Flags, int N) { return ((Flags >> N) & 1) != 0; }

    // a file spec as the mod tools write it: its folders with backslashes, the exported extension
    std::string SpecPath(std::string Name, const std::string& Extension)
    {
        if (Name.empty()) return Name;
        std::replace(Name.begin(), Name.end(), '/', '\\');
        const size_t Dot = Name.find('.', Name.find_last_of('\\') == std::string::npos ? 0 : Name.find_last_of('\\'));
        if (Dot != std::string::npos) Name.resize(Dot);
        return Name + Extension;
    }
}

void Bo3ReadSoundFileSpecs(const SoundAliasCsv::ZoneView& Z, int64_t Bank, std::unordered_map<uint32_t, std::string>& Out)
{
    const uint32_t AliasCount = Z.At<uint32_t>(Bank + AliasCountAt);
    const int64_t Aliases = Z.Follow(Bank + AliasesAt);
    for (uint32_t I = 0; Aliases >= 0 && I < AliasCount; I++)
    {
        const int64_t A = Aliases + I * AliasSize;
        const uint32_t Count = Z.At<uint32_t>(A + 0x18);
        const int64_t Entries = Z.Follow(A + 0x10);
        for (uint32_t J = 0; Entries >= 0 && J < Count; J++)
            for (int K = 0; K < 3; K++)
            {
                const int64_t F = Entries + J * EntrySize + 0x38 + K * 0x10;
                const uint32_t Hash = Z.At<uint32_t>(F);
                if (Hash == 0) continue;
                std::string Path = Z.Text(F + 8);
                if (!Path.empty()) Out.emplace(Hash, std::move(Path));
            }
    }
}

void Bo3ReadSoundAliases(const SoundAliasCsv::ZoneView& Z, int64_t Bank, const std::unordered_map<uint32_t, std::string>& Exported,
                         const std::string& Extension, SoundAliasCsv::Rows& Out)
{
    auto& N = Out.Counters;
    Treyarch::SoundNames Names;
    Names.Add(KnownNames);
    Names.Add(Curves);

    const uint32_t AliasCount = Z.At<uint32_t>(Bank + AliasCountAt);
    const int64_t Aliases = Z.Follow(Bank + AliasesAt);
    if (AliasCount == 0) return;
    if (Aliases < 0)
    {
        N["alias_lists_unread"] += AliasCount;
        return;
    }
    // the zone's own names first: what a hash field names is mostly another alias or duck of the bank
    std::string Last;
    std::vector<std::string> ListNames(AliasCount);
    for (uint32_t I = 0; I < AliasCount; I++)
    {
        const int64_t A = Aliases + I * AliasSize;
        std::string Name = Z.Text(A);
        if (Name.empty()) Name = Last;   // a later alias of the same name points to no string of its own
        Names.Add(Z.At<uint32_t>(A + 8), Name);
        ListNames[I] = Last = Name;
    }
    const uint32_t DuckCount = Z.At<uint32_t>(Bank + DuckCountAt);
    const int64_t Ducks = Z.Follow(Bank + DucksAt);
    for (uint32_t I = 0; Ducks >= 0 && I < DuckCount; I++)
    {
        const int64_t D = Ducks + I * DuckSize;
        const std::string Name = Z.Chars(D, 64);
        if (Treyarch::SoundHash(Name) == Z.At<uint32_t>(D + 0x40)) Names.Add(Name);
        else N["alias_ducks_unnamed"]++;
    }

    const auto Hashed = [&](uint32_t Hash, const char* Counter) {
        bool Known = true;
        std::string S = Names.Of(Hash, Known);
        if (!Known) N[Counter]++;
        return S;
    };
    const auto Enum = [&](const std::string& S, const char* Counter) {
        if (S.empty()) N[Counter]++;
        return S;
    };

    for (uint32_t I = 0; I < AliasCount; I++)
    {
        const int64_t A = Aliases + I * AliasSize;
        const uint32_t Count = Z.At<uint32_t>(A + 0x18);
        const int64_t Entries = Z.Follow(A + 0x10);
        if (Entries < 0)
        {
            N["alias_lists_unread"]++;
            continue;
        }
        N["alias_lists"]++;
        for (uint32_t J = 0; J < Count; J++)
        {
            const int64_t E = Entries + J * EntrySize;
            SoundAliasCsv::Alias R;
            R[Col::Name] = ListNames[I];

            // the file specs {hash 0, name pointer 8} at 0x38, 0x48 (sustain), 0x58 (release)
            const Col Specs[] = { Col::FileSpec, Col::FileSpecSustain, Col::FileSpecRelease };
            for (int K = 0; K < 3; K++)
            {
                const int64_t F = E + 0x38 + K * 0x10;
                const uint32_t Hash = Z.At<uint32_t>(F);
                const auto It = Exported.find(Hash);
                if (Hash != 0 && It != Exported.end()) R[Specs[K]] = It->second;
                else R[Specs[K]] = SpecPath(Z.Text(F + 8), Extension);
            }
            // strings: subtitle 0x10, secondary {pointer 0x18, hash 0x20}, stop alias {0x28, 0x30}
            R[Col::Subtitle] = Z.Text(E + 0x10);
            R[Col::Secondary] = Z.Text(E + 0x18);
            if (R[Col::Secondary].empty()) R[Col::Secondary] = Hashed(Z.At<uint32_t>(E + 0x20), "alias_secondaries_unnamed");
            R[Col::StopAlias] = Z.Text(E + 0x28);
            if (R[Col::StopAlias].empty()) R[Col::StopAlias] = Hashed(Z.At<uint32_t>(E + 0x30), "alias_stop_aliases_unnamed");

            const uint32_t F0 = Z.At<uint32_t>(E + 0x68), F1 = Z.At<uint32_t>(E + 0x6c);
            R[Col::Looping] = Bit(F0, 0) ? "looping" : "nonlooping";
            R[Col::PanType] = Bit(F0, 1) ? "3d" : "2d";
            R[Col::SilentInCPZ] = SoundAliasCsv::YesNo(Bit(F0, 2));
            R[Col::ContextFailsafe] = SoundAliasCsv::YesNo(Bit(F0, 3));
            R[Col::DistanceLpf] = SoundAliasCsv::YesNo(Bit(F0, 4));
            R[Col::Doppler] = SoundAliasCsv::YesNo(Bit(F0, 5));
            R[Col::Pauseable] = SoundAliasCsv::YesNo(Bit(F0, 6));
            R[Col::IsMusic] = SoundAliasCsv::YesNo(Bit(F0, 7));
            R[Col::StopOnEntDeath] = SoundAliasCsv::YesNo(Bit(F0, 8));
            R[Col::Timescale] = SoundAliasCsv::YesNo(Bit(F0, 9));
            R[Col::VoiceLimit] = SoundAliasCsv::YesNo(Bit(F0, 10));
            R[Col::IgnoreMaxDist] = SoundAliasCsv::YesNo(Bit(F0, 11));
            R[Col::Storage] = Enum(SoundAliasCsv::Named((F0 >> 12) & 3, StorageTypes), "alias_enums_unknown");
            R[Col::FluxType] = SoundAliasCsv::Named((F0 >> 14) & 0xf, FluxTypes);
            R[Col::LimitType] = Enum(SoundAliasCsv::Named((F0 >> 18) & 3, LimitTypes), "alias_enums_unknown");
            R[Col::EntityLimitType] = Enum(SoundAliasCsv::Named((F0 >> 20) & 3, LimitTypes), "alias_enums_unknown");
            R[Col::GPAD] = SoundAliasCsv::YesNo(Bit(F0, 25));
            R[Col::GPADOnly] = SoundAliasCsv::YesNo(Bit(F0, 26));
            R[Col::MuteVoice] = SoundAliasCsv::YesNo(Bit(F0, 27));
            R[Col::MuteMusic] = SoundAliasCsv::YesNo(Bit(F0, 28));
            R[Col::NeverPlayTwice] = SoundAliasCsv::YesNo(Bit(F1, 0));
            R[Col::DryMaxCurve] = Enum(SoundAliasCsv::Named((F1 >> 2) & 0x3f, Curves), "alias_enums_unknown");
            R[Col::WetMaxCurve] = Enum(SoundAliasCsv::Named((F1 >> 8) & 0x3f, Curves), "alias_enums_unknown");
            R[Col::DryMinCurve] = Enum(SoundAliasCsv::Named((F1 >> 14) & 0x3f, Curves), "alias_enums_unknown");
            R[Col::WetMinCurve] = Enum(SoundAliasCsv::Named((F1 >> 20) & 0x3f, Curves), "alias_enums_unknown");
            R[Col::IsCinematic] = SoundAliasCsv::YesNo(Bit(F1, 27));
            R[Col::IsBig] = SoundAliasCsv::YesNo(Bit(F1, 28));
            R[Col::AmplitudePriority] = SoundAliasCsv::YesNo(Bit(F1, 29));
            R[Col::ContinuousPan] = SoundAliasCsv::YesNo(Bit(F1, 30));
            R[Col::RestartContextLoops] = SoundAliasCsv::YesNo(Bit(F1, 31));
            // bits no column is known for (RandomizeType is not among 22-24: they follow neither a pitch,
            // a volume range nor a variant count), set: counted
            for (int K = 0; K < 32; K++)
            {
                if (Bit(F0 & 0xe1c00000u, K)) N["alias_flags0_bit" + std::to_string(K)]++;
                if (Bit(F1 & 0x04000002u, K)) N["alias_flags1_bit" + std::to_string(K)]++;
            }

            // hashes: duck 0x70, four contexts {type, value} from 0x74, stop on play 0x94, futz 0x98
            R[Col::Duck] = Hashed(Z.At<uint32_t>(E + 0x70), "alias_hashes_unknown");
            const Col Contexts[] = { Col::ContextType, Col::ContextValue, Col::ContextType1, Col::ContextValue1,
                                     Col::ContextType2, Col::ContextValue2, Col::ContextType3, Col::ContextValue3 };
            for (int K = 0; K < 8; K++) R[Contexts[K]] = Hashed(Z.At<uint32_t>(E + 0x74 + K * 4), "alias_hashes_unknown");
            R[Col::StopOnPlay] = Hashed(Z.At<uint32_t>(E + 0x94), "alias_hashes_unknown");
            R[Col::FutzPatch] = Hashed(Z.At<uint32_t>(E + 0x98), "alias_hashes_unknown");

            // linear gains: reverb send 0x9c, center send 0xa0, volumes 0xa4/0xa8, envelope 0xac
            R[Col::ReverbSend] = SoundAliasCsv::Volume(Z.At<float>(E + 0x9c));
            R[Col::CenterSend] = SoundAliasCsv::Volume(Z.At<float>(E + 0xa0));
            R[Col::VolMin] = SoundAliasCsv::Volume(Z.At<float>(E + 0xa4));
            R[Col::VolMax] = SoundAliasCsv::Volume(Z.At<float>(E + 0xa8));
            R[Col::EnvelopPercent] = SoundAliasCsv::Volume(Z.At<float>(E + 0xac));
            R[Col::FluxTime] = std::to_string(Z.At<int16_t>(E + 0xb0));
            R[Col::StartDelay] = std::to_string(Z.At<int16_t>(E + 0xb2));
            // pitch: u16, 32767 = unchanged (OAT's T6 scale; HydraX's 32768 reads "-0.05" for 0)
            R[Col::PitchMin] = SoundAliasCsv::CentsU16(Z.At<uint16_t>(E + 0xb4), 32767.0);
            R[Col::PitchMax] = SoundAliasCsv::CentsU16(Z.At<uint16_t>(E + 0xb6), 32767.0);
            // distances stored halved (u16: an odd distance reads one less), the envelope's too
            R[Col::DistMin] = std::to_string(Z.At<uint16_t>(E + 0xb8) * 2);
            R[Col::DistMaxDry] = std::to_string(Z.At<uint16_t>(E + 0xba) * 2);
            R[Col::DistMaxWet] = std::to_string(Z.At<uint16_t>(E + 0xbc) * 2);
            R[Col::EnvelopMin] = std::to_string(Z.At<uint16_t>(E + 0xbe) * 2);
            R[Col::EnvelopMax] = std::to_string(Z.At<uint16_t>(E + 0xc0) * 2);
            R[Col::DistanceLpfMin] = std::to_string(Z.At<int16_t>(E + 0xc2));
            R[Col::DistanceLpfMax] = std::to_string(Z.At<int16_t>(E + 0xc4));
            R[Col::FadeIn] = std::to_string(Z.At<int16_t>(E + 0xc6));
            R[Col::FadeOut] = std::to_string(Z.At<int16_t>(E + 0xc8));
            R[Col::DopplerScale] = std::to_string(Z.At<int16_t>(E + 0xca));
            // bytes: thresholds, probability, occlusion as 0..1; priorities, counts as they are
            R[Col::PriorityThresholdMin] = SoundAliasCsv::Fraction(Z.At<uint8_t>(E + 0xcc));
            R[Col::PriorityThresholdMax] = SoundAliasCsv::Fraction(Z.At<uint8_t>(E + 0xcd));
            R[Col::Probability] = SoundAliasCsv::Fraction(Z.At<uint8_t>(E + 0xce));
            R[Col::OcclusionLevel] = SoundAliasCsv::Fraction(Z.At<uint8_t>(E + 0xcf));
            R[Col::PriorityMin] = std::to_string(Z.At<uint8_t>(E + 0xd0));
            R[Col::PriorityMax] = std::to_string(Z.At<uint8_t>(E + 0xd1));
            R[Col::Pan] = Enum(SoundAliasCsv::Named(Z.At<uint8_t>(E + 0xd2), Pans), "alias_enums_unknown");
            R[Col::LimitCount] = std::to_string(Z.At<uint8_t>(E + 0xd3));
            R[Col::EntityLimitCount] = std::to_string(Z.At<uint8_t>(E + 0xd4));
            R[Col::DuckGroup] = Enum(SoundAliasCsv::Named(Z.At<uint8_t>(E + 0xd5), DuckGroups), "alias_enums_unknown");
            R[Col::Bus] = Enum(SoundAliasCsv::Named(Z.At<uint8_t>(E + 0xd6), Buses), "alias_enums_unknown");
            R[Col::VolumeGroup] = Enum(SoundAliasCsv::Named(Z.At<uint8_t>(E + 0xd7), VolumeGroups), "alias_enums_unknown");
            Out.Aliases.push_back(std::move(R));
        }
    }
}
