#pragma once

// The mod's defaults: Homard's own settings (bin\fc5vr_settings.txt as he left it on 20 Oct 2026 18:41), shipped in
// release pre_alpha_0_1 (Homard 20 Oct: "make all my current settings the default"). Replayed at start before
// fc5vr_settings.txt, which overrides them line by line; written as fc5vr_settings.txt when there is none, so a fresh
// file is exactly this text. tools\release\fc5_package.ps1 checks the shipped file against it.
// Release 0.12.1 (Homard 25 Oct 12:40: "make sure that experimental melee and throwable are off by default"): throwable off.
// 29 Oct (Homard 13:49: "did you also brought all the different parameters into the unified system"): bodyview on, one switch for
// viewpivot, viewroot, viewrootsmooth, rootsteady and walkbob (no longer saved on their own); steadyview retired (off since 12 Oct).
// 28 Oct (Homard 16:44: "have all the weapon right position saved when we ship the mod"): the sight lines (sightcal) and
// scope zooms (scopecal) learned for 17 more weapons in the simulator on the "All Weapons YT" Arcade map
// (tools\arcade\allweapons.ps1, testruns\2026-10-28_arcade). They are read from the game's own aim-down-sights pose of the
// map's version of each gun (its attachments): a gun whose optic differs from the map's needs its own line (see STATUS).
// 29 Oct (Homard 13:48, guns turned and an aim down sights near the eye): gunaxes, adsprobe optic, scopesticky, weaponidsplit.
// 30 Oct (Homard 13:46: "make frame pacing off by default"): framelock off. Homard 13:58, "weapon pitch on the controller" set in his
// 13:25 play (not saved then): grippitch -3.0; 15:09 (release 0.15): -5.0.
namespace fc5 {
inline constexpr const char* kReleaseSettings = R"FC5VR(
# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).
adsprobe optic
aimdot on
aimlead 1.0
airfollow on
airroll 1.00
aniso 16
aoexact on
autorecenter on
autorecenterdist 0.50
autorecenterwait 0.5
bbcopyskip on
bodyview on
bulletspeed 1.00
cinematics 3d
decaleye on
dlaaphases 16
dlss on
dlsscammv 1
dlssmvbest on
dlssmvcheck on
dlsspreset 11
dlsssharp 0.30
dlsstrim 7
eyeprobeoff on
f11image off
firegate 0.10
firemeasure on
firesplit on
fovexact on
fovmargin 0
fovnarrow 1.00
framedouble on
framelock off
gesturebuzz on
gputime on
grippitch -5.0
gunanchor on
gunaxes on
gunstill on
gunstillrecoil off
hands on
hidetracers on
hipspread 0.00
huddist 2.27
hudfullres on
hudheight -0.16
hudmarkcap on
hudmarkdist 8
hudmarkers on
hudmarkicon 2.0
hudmarkmax 4.0
hudmarksize 0.21
hudmarksmooth 0.3
hudmask on
hudmode 1
hudmul on
hudprim on
hudprimcam on
hudscreen on
hudsize 40
lakeoverscan 1.00
lakeshared on
logbuffered on
menulevel 1
mipbias 0
mipbiasauto on
norecoil on
nosway on
pairjumps on
postclean on
prevmvonce on
queryvisible on
reddot on
reddotonly on
reddotsize 9.0
reflections on
render stereo
resscale 1.0
savelater on
scope on
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 18912
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 24417
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 3168
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32325
scopecal 0.0000 0.0750 0.0000 0.0180 1.00 40362
scopecal 0.0000 0.0750 0.0000 0.0180 3.90 4176
scopecal 0.0000 0.0750 0.0000 0.0180 12.00 42000
scopecal 0.0000 0.0750 0.0000 0.0180 1.00 57819
scopecal 0.0000 0.0640 0.0740 0.0180 3.30 61926
scopecal 0.0000 0.0640 0.0740 0.0180 1.30 62229
scopecal 0.0000 0.0640 0.0740 0.0180 3.30 23397
scopecal 0.0000 0.0640 0.0740 0.0180 3.70 23475
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 25110
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 27144
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32574
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32688
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 35499
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 35709
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 37359
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 39012
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 39408
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 41649
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 45582
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 58416
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 65535
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 30567
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 42549
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 63120
scopeevery 1
scopeeye right
scopeglassat -0.0032 0.0279 0.0734 0.0254 42000
scopeglassat -0.0457 0.0677 0.0759 0.0069 61926
scopeglassat -0.0397 0.0805 0.0730 0.0144 62229
scopehistswap on
scopemodel on
scopepass on
scoperecoiloff off
scoperes 1024
scopeskip 15
scopesrc 1
scopesticky on
shadowkernel on
shadowmv on
shadowmvnew 0.05
shoulderswitch on
shoulderswitchsize 0.20
sightaim on
sightcal 18912 0.00017 -0.31300 0.09752 -0.000599 0.999975 -0.007056 0.00019 0.00706 0.99998
sightcal 23397 -0.00096 -0.12421 0.17628 0.000779 0.999999 -0.001010 -0.00002 0.00101 1.00000
sightcal 24417 0.00288 -0.40229 0.11493 -0.003211 0.999993 0.001732 0.00000 -0.00173 1.00000
sightcal 25110 -0.03693 -0.34054 0.13312 0.000456 0.999996 0.002811 -0.17349 -0.00269 0.98483
sightcal 27144 0.00029 0.02679 0.09844 -0.000236 1.000000 -0.000425 0.00016 0.00042 1.00000
sightcal 30567 -0.00036 -0.35867 0.10227 0.000087 0.999999 -0.001087 0.00016 0.00109 1.00000
sightcal 32325 0.00030 -0.34300 0.14585 -0.000329 1.000000 -0.000696 0.00015 0.00070 1.00000
sightcal 32574 -0.00883 -0.42876 0.14808 0.011666 0.998188 -0.059028 -0.20493 0.06017 0.97693
sightcal 32688 0.00000 -0.04873 0.08200 0.000058 1.000000 0.000731 0.00010 -0.00073 1.00000
sightcal 35499 -0.00004 -0.06476 0.12430 -0.000045 0.999757 -0.022038 0.00022 0.02204 0.99976
sightcal 35709 0.00013 -0.14050 0.18108 -0.000126 0.999957 -0.009237 0.00013 0.00924 0.99996
sightcal 37359 0.00023 -0.14049 0.18103 -0.000352 0.999958 -0.009127 0.00013 0.00913 0.99996
sightcal 39012 0.00004 -0.02132 0.08645 0.000057 1.000000 0.000411 0.00015 -0.00041 1.00000
sightcal 39408 -0.00003 -0.08059 0.10044 0.000109 0.999956 -0.009354 0.00017 0.00935 0.99996
sightcal 40362 -0.00016 -0.09505 0.15979 0.000364 0.999999 -0.001019 0.00021 0.00102 1.00000
sightcal 41649 -0.00000 -0.31783 0.11355 0.000111 1.000000 0.000415 0.00023 -0.00042 1.00000
sightcal 42000 -0.00119 -0.11874 0.17296 0.000153 1.000000 -0.000559 0.00027 0.00056 1.00000
sightcal 42549 0.00013 -0.01433 0.14499 -0.000151 0.999836 -0.018091 0.00020 0.01809 0.99984
sightcal 45582 0.00029 -0.02630 0.14914 -0.000382 0.999974 -0.007129 0.00010 0.00713 0.99997
sightcal 57819 -0.00033 -0.09603 0.16667 -0.000097 1.000000 -0.000221 0.00018 0.00022 1.00000
sightcal 58416 0.00001 -0.19348 0.15901 -0.000030 0.999979 0.006498 -0.00009 -0.00650 0.99998
sightcal 61926 0.00051 -0.30190 0.21349 -0.000333 1.000000 0.000102 0.00021 -0.00010 1.00000
sightcal 62229 -0.00068 -0.18176 0.16382 -0.000116 0.999999 -0.001532 0.00019 0.00153 1.00000
sightcal 63120 -0.00024 -0.20207 0.19182 0.000249 0.999934 0.011472 0.00029 -0.01147 0.99993
sightcal 65535 0.00135 -0.10057 0.19188 -0.000007 0.999697 -0.024620 0.00011 0.02462 0.99970
skipps FlareDiffuseSampler
slingpos 0.09 0.50 0.05
slingrot 70 -30 0
smoothmotion on
snapresetfix on
snapsync on
ssrjitterpair on
ssrmv off
ssrtileclear on
steadyexact on
taamv on
taaskip on
throwable off
tracersfromgun on
upright on
vehfade off
vehicleperpair on
vehicles on
vehplayerfov on
vehrecenter on
weaponcal 0 2 29841
weaponidsplit on
weaponsmooth 0.20
wheelstick on
)FC5VR";
}
