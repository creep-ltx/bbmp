// SPDX-License-Identifier: GPL-3.0-or-later
// Songs on Modland whose settings are known. A GoatTracker song doesn't say how often
// per frame it is to be called, for which SID it was made, or which options the editor
// had; the C64 program made from it does. tools of the scratch folder compared, for every
// song that the High Voltage SID Collection (HVSC #83) has as a program, the values this
// player writes to the SID with the values the program writes (run on a 6502 emulation),
// call by call over 3000 calls, for all settings, and kept the settings that agree.
//
// hash (FNV-1a 64 of the file), calls per frame, SID model, hard restart ADSR,
// flags (1 pulse skipped on a note's first frame, 2 effects skipped, 4 fine vibrato),
// what the comparison gave
    // GoatTracker 2/Cadaver/covert ops in 2d (funktempo).sng
    {0x6f2a8bcb25ed0744ULL, 1, 6581, 0x0f00, 6, "another version of HVSC /MUSICIANS/C/Cadaver/Covert_Ops_in_2D.sid: same notes, SID registers agree in 0.1% of 3000 calls (what can be heard of them: 2.5%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/Cadaver/dojo.sng
    {0x1e3eb9bbf9d884eaULL, 1, 6581, 0x0f00, 7, "another version of HVSC /MUSICIANS/C/Cadaver/Dojo.sid: same notes, SID registers agree in 9.9% of 3000 calls (what can be heard of them: 61.6%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/Cadaver/on a sanction from cia.sng
    {0x81d377434ba19913ULL, 1, 6581, 0x0f00, 7, "another version of HVSC /MUSICIANS/C/Cadaver/On_a_Sanction_from_CIA.sid: same notes, SID registers agree in 4.5% of 3000 calls (what can be heard of them: 4.5%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/Cadaver/unleash the f...... fury.sng
    {0xd7b5b5242bc37325ULL, 1, 8580, 0x0f00, 6, "another version of HVSC /MUSICIANS/C/Cadaver/Unleash_the_Fucking_Fury.sid: same notes, SID registers agree in 9.1% of 3000 calls (what can be heard of them: 23.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/Hein/ghosttrackers.sng
    {0x5b76bc59c3f83025ULL, 1, 6581, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/H/Holt_Hein/GhostTrackers.sid in 100% of 3000 calls"},
    // GoatTracker 2/Jammer/my own hyperspace.sng
    {0xcefc1bfe8290163dULL, 1, 8580, 0x0ff0, 7, "another version of HVSC /MUSICIANS/J/Jammer/My_Own_Hyperspace.sid: same notes, SID registers agree in 78.2% of 3000 calls (what can be heard of them: 92.4%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/Kalachnikov/sid-warrior.sng
    {0xda9e4b685b5fa6ddULL, 1, 6581, 0x0f00, 7, "SID registers as HVSC /DEMOS/S-Z/Sid-Warrior.sid in 100.0% of 3000 calls"},
    // GoatTracker 2/Nata/stargate.sng
    {0xcd02465363c28fd7ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/N/Nata/Stargate.sid: same notes, SID registers agree in 73.2% of 3000 calls (what can be heard of them: 85.2%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker 2/No-XS/sixpack of cola.sng
    {0xd35acd2d3e5a91abULL, 1, 6581, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/N/No-XS/Sixpack_of_Cola.sid in 100.0% of 3000 calls"},
    // GoatTracker 2/Randall/i'll be a pimp in cabrini green (when i grow up).sng
    {0xbb712950699b10a8ULL, 1, 8580, 0x0ff0, 3, "SID registers as HVSC /MUSICIANS/H/Hoffmann_Michal/Ill_Be_a_Pimp_in_Cabrini_Green_when_I_grow_up.sid in 100.0% of 3000 calls"},
    // GoatTracker 2/Richard Bayliss/everlasting annoyance.sng
    {0x2e6abd62a3272336ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/B/Bayliss_Richard/Everlasting_Annoyance.sid: same notes, SID registers agree in 23.7% of 3000 calls (what can be heard of them: 49.5%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/alien funk.sng
    {0x18dd97b4b76bc50bULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Alien_Funk.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/attitude 14.sng
    {0x903cd08d937a9e5dULL, 1, 8580, 0x0f00, 6, "SID registers as HVSC /MUSICIANS/M/MCH/Attitude_14.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/balcony princess.sng
    {0x5032267e5bf811ecULL, 1, 8580, 0x0f00, 6, "another version of HVSC /MUSICIANS/M/MCH/Balcony_Princess.sid: same notes, SID registers agree in 0.6% of 3000 calls (what can be heard of them: 97.8%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/coop-Stinsen/illusions x2.sng
    {0x274bf2cacd848647ULL, 2, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/M/MCH/Illusions.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/coop-Stinsen/mr fourex x4.sng
    {0xa198ece38ace7bb6ULL, 4, 8580, 0x0ff0, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Mr_Fourex.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/golden moments.sng
    {0x86ca2542f3912598ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Golden_Moments.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/graffiti intro.sng
    {0x26041cc29eebd0a4ULL, 1, 8580, 0xff00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Graffiti_Intro.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/groove machine.sng
    {0x12583cba0ea2db46ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/M/MCH/Groove_Machine.sid: same notes, SID registers agree in 0.2% of 3000 calls (what can be heard of them: 10.4%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/gubbdata 2016 invitation.sng
    {0x9d54ee4c6b46ef9fULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Gubbdata_2016_Invite.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/homunculi tune 1.sng
    {0x3db6161e455d0c74ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Homunculi_tune_1.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/homunculi tune 2.sng
    {0x359899c05c29374fULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Homunculi_tune_2.sid in 99.8% of 3000 calls"},
    // GoatTracker/Mch/hybrid song.sng
    {0x8b2cca4a71bf0d88ULL, 1, 8580, 0x0f00, 5, "another version of HVSC /MUSICIANS/M/MCH/Hybrid_Song_Funky_Stars.sid: same notes, SID registers agree in 91.4% of 3000 calls (what can be heard of them: 100.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/in a rush.sng
    {0x9c7e8acae6d88b0fULL, 1, 8580, 0x0f00, 6, "SID registers as HVSC /MUSICIANS/M/MCH/In_A_Rush.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/midnight dream.sng
    {0xce3b949b5538c1b1ULL, 1, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/M/MCH/Midnight_Dream.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/on the road once again.sng
    {0xd011b03ba9488ab0ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/M/MCH/On_the_Road_Once_Again.sid: same notes, SID registers agree in 0.2% of 3000 calls (what can be heard of them: 99.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/pagans mind.sng
    {0xcf5dc3e6c4b40cedULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Pagans_Mind.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/plasma intro.sng
    {0xfdffbff35070629fULL, 1, 8580, 0x0ff0, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Plasma_Intro.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/pocket tomb.sng
    {0xa851a15fa6a9ba98ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/M/MCH/Pocket_Tomb.sid: same notes, SID registers agree in 96.6% of 3000 calls (what can be heard of them: 97.7%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Mch/sky captain.sng
    {0x2ccc9e51fbef37f2ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/M/MCH/Sky_Captain.sid in 100.0% of 3000 calls"},
    // GoatTracker/Mch/two years in gp.sng
    {0x9318ebfbcc74ccbfULL, 1, 8580, 0x0ff0, 7, "another version of HVSC /MUSICIANS/M/MCH/Two_Years_in_G_P.sid: same notes, SID registers agree in 1.0% of 3000 calls (what can be heard of them: 95.7%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/alcorythm_ffff.sng
    {0x3a8393c68c3693e9ULL, 1, 8580, 0xffff, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Alcorythm.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/archaic_groove.sng
    {0x09089e565d86d7d5ULL, 1, 8580, 0xffff, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Archaic_Groove.sid in 99.9% of 3000 calls"},
    // GoatTracker/Stinsen/arpling.sng
    {0x12a1a7a6e14ec8a8ULL, 1, 8580, 0x0f00, 5, "another version of HVSC /MUSICIANS/S/Stinsen/Ugly_Arpling.sid: same notes, SID registers agree in 82.3% of 3000 calls (what can be heard of them: 83.8%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/bait.sng
    {0x723c94aa08237f33ULL, 1, 8580, 0x0f00, 4, "another version of HVSC /MUSICIANS/S/Stinsen/Bait_and_Switch.sid: same notes, SID registers agree in 87.1% of 3000 calls (what can be heard of them: 100.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/ballad.sng
    {0x5703ebfcb1092805ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Worktunes/Ballad.sid: same notes, SID registers agree in 81.2% of 3000 calls (what can be heard of them: 81.9%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/christmas_boogie.sng
    {0x75ca2b41cc8e0b00ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Christmas_Boogie.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/classical.sng
    {0x9fe5c7326ad20c4bULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Classical.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/coconut_conundrum.sng
    {0xd0fe8f06f2dc77efULL, 1, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Coconut_Conundrum.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/defunkt_final_fv_po_ro_6581_ffff.sng
    {0xe9baa6aec2c31055ULL, 1, 6581, 0xffff, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Defunkt.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/diminishing_returns.sng
    {0x0f955dca64351a91ULL, 1, 8580, 0xffff, 4, "another version of HVSC /MUSICIANS/S/Stinsen/Diminishing_Returns.sid: same notes, SID registers agree in 24.9% of 3000 calls (what can be heard of them: 24.9%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/double_rainbow_2x.sng
    {0xa05f823d5cfbf2b1ULL, 2, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Double_Rainbow.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/endless_summer.sng
    {0xc88a11a696eeaea3ULL, 1, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Endless_Summer.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/failedfunk.sng
    {0x3af687669f51238dULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Failedfunk.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/feeling_pink.sng
    {0xf73951b36dc0449fULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Feeling_Pink.sid in 99.8% of 3000 calls"},
    // GoatTracker/Stinsen/flumbos_keps.sng
    {0x441bc273974e53b6ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Worktunes/Flumbos_Keps.sid: same notes, SID registers agree in 43.5% of 3000 calls (what can be heard of them: 49.3%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/forced_entry.sng
    {0x064febcddfade154ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Forced_Entry.sid in 98.7% of 3000 calls"},
    // GoatTracker/Stinsen/game_tune.sng
    {0xd8898c6e8fdc1bf3ULL, 1, 8580, 0x0f00, 7, "not in HVSC under this name; SID model as the author's other songs, the rest from the file's name or the editor's settings"},
    // GoatTracker/Stinsen/gremlinfunk.sng
    {0x199fa412f5c4d2fbULL, 1, 8580, 0x0f00, 5, "SID registers as HVSC /MUSICIANS/S/Stinsen/Gremlinfunk.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/iguana.sng
    {0x9ed5d28baa5f2366ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Iguana.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/jeroenimo.sng
    {0x4fae634c74514470ULL, 1, 8580, 0x0f00, 7, "not in HVSC under this name; SID model as the author's other songs, the rest from the file's name or the editor's settings"},
    // GoatTracker/Stinsen/jogeir.sng
    {0x1b73a39391306f55ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Jogeir.sid in 99.9% of 3000 calls"},
    // GoatTracker/Stinsen/lethargic.sng
    {0x15eb214940d22cf9ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Lethargic.sid: same notes, SID registers agree in 95.9% of 3000 calls (what can be heard of them: 96.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/lolo_sawtooth_only.sng
    {0x0df487988030d235ULL, 1, 8580, 0x0f00, 7, "not in HVSC under this name; SID model as the author's other songs, the rest from the file's name or the editor's settings"},
    // GoatTracker/Stinsen/nordic_scene_review.sng
    {0x00b10561e51875d4ULL, 1, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Nordic_Scene_Review.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/pirate.sng
    {0x2ced161df330a242ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Pirate.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/premonition_fv_po_ro_ffff.sng
    {0xdb2766cc2e88e708ULL, 1, 8580, 0xffff, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Premonition.sid in 99.9% of 3000 calls"},
    // GoatTracker/Stinsen/quaralline_2x.sng
    {0xff7546e8bd2f0973ULL, 2, 8580, 0xffff, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Quaralline.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/relapse.sng
    {0x71ddd4d976936b04ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Relapse.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/reverb.sng
    {0xb456cbbf2807bdf1ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Reverb.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/running_on_empty_ffff.sng
    {0x2b97fe9e5d31feb7ULL, 1, 8580, 0xffff, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Running_on_Empty.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/safari.sng
    {0x1aba24b8e4f24b1aULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Safari.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/scarabaeus_0f00_fv_po_ro.sng
    {0x2fc2b33da0b63de3ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Scarabaeus.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/shadow.sng
    {0xaa1eca058a90986aULL, 1, 8580, 0x0f00, 7, "not in HVSC under this name; SID model as the author's other songs, the rest from the file's name or the editor's settings"},
    // GoatTracker/Stinsen/sidetracked.sng
    {0xe2a1a0268e4302e5ULL, 1, 8580, 0x0f00, 4, "SID registers as HVSC /MUSICIANS/S/Stinsen/Sidetracked.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/sniff.sng
    {0x9f08176f41bed26eULL, 1, 8580, 0x0000, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Worktunes/Sniff.sid: same notes, SID registers agree in 15.8% of 3000 calls (what can be heard of them: 37.0%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/space_2x.sng
    {0x0657bab12a346961ULL, 2, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Space_Cheddar.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/sunset_shuffle_po_ro_ffff.sng
    {0x6f3dca7e59f9b809ULL, 1, 8580, 0xffff, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Sunset_Shuffle.sid: same notes, SID registers agree in 37.6% of 3000 calls (what can be heard of them: 37.6%); SID model and speed from there, settings from the file's name"},
    // GoatTracker/Stinsen/trainwreck.sng
    {0xada3bc46b90beefcULL, 1, 8580, 0xffff, 5, "another version of HVSC /MUSICIANS/S/Stinsen/Trainwreck.sid: same notes, SID registers agree in 94.5% of 3000 calls (what can be heard of them: 94.6%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/tribal.sng
    {0x295f6a10c82eae26ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Tribal.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/tribal18.sng
    {0x7116cd419b06e081ULL, 1, 8580, 0x0f00, 7, "SID registers as HVSC /MUSICIANS/S/Stinsen/Worktunes/Tribal18.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/tribal_tribunal.sng
    {0xcfe26bbdc68b8705ULL, 1, 8580, 0x0f00, 7, "another version of HVSC /MUSICIANS/S/Stinsen/Tribal_Tribunal.sid: same notes, SID registers agree in 95.5% of 3000 calls (what can be heard of them: 95.5%); SID model and speed from there, settings of the best agreement"},
    // GoatTracker/Stinsen/unleash_the_cheese.sng
    {0xd8b3f4a9c1554e0cULL, 1, 8580, 0xffff, 5, "SID registers as HVSC /MUSICIANS/S/Stinsen/Unleash_the_Cheese.sid in 100.0% of 3000 calls"},
    // GoatTracker/Stinsen/upsandowns.sng
    {0x19d6366f4f5ffec8ULL, 1, 8580, 0x0f00, 7, "not in HVSC under this name; SID model as the author's other songs, the rest from the file's name or the editor's settings"},
