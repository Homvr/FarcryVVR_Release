#pragma once

// The mod's defaults: Homard's own settings (bin\fc5vr_settings.txt as he left it on 20 Oct 2026 18:41), shipped in
// release pre_alpha_0_1 (Homard 20 Oct: "make all my current settings the default"). Replayed at start before
// fc5vr_settings.txt, which overrides them line by line; written as fc5vr_settings.txt when there is none, so a fresh
// file is exactly this text. tools\release\fc5_package.ps1 checks the shipped file against it.
// Release 0.12.1 (Homard 25 Oct 12:40: "make sure that experimental melee and throwable are off by default"): throwable off.
namespace fc5 {
inline constexpr const char* kReleaseSettings = R"FC5VR(
# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).
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
gesturebuzz on
gputime on
gunanchor on
gunstill on
gunstillrecoil off
hands on
hidetracers on
hipspread 0.00
huddist 2.27
hudfullres on
hudheight -0.16
hudmarkdist 8
hudmarkers on
hudmarksize 0.21
hudmarksmooth 0.3
hudmask on
hudmode 1
hudmul on
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
shadowkernel on
shadowmv on
shadowmvnew 0.05
shoreblur on
shoreblurpx 3
shoulderswitch on
shoulderswitchsize 0.20
sightaim on
sightcal 18912 0.00017 -0.31300 0.09752 -0.000599 0.999975 -0.007056 0.00019 0.00706 0.99998
sightcal 24417 0.00288 -0.40229 0.11493 -0.003211 0.999993 0.001732 0.00000 -0.00173 1.00000
sightcal 32325 0.00030 -0.34300 0.14585 -0.000329 1.000000 -0.000696 0.00015 0.00070 1.00000
sightcal 40362 -0.00016 -0.09505 0.15979 0.000364 0.999999 -0.001019 0.00021 0.00102 1.00000
sightcal 42000 -0.00119 -0.11874 0.17296 0.000153 1.000000 -0.000559 0.00027 0.00056 1.00000
sightcal 57819 -0.00033 -0.09603 0.16667 -0.000097 1.000000 -0.000221 0.00018 0.00022 1.00000
sightcal 61926 0.00051 -0.30190 0.21349 -0.000333 1.000000 0.000102 0.00021 -0.00010 1.00000
sightcal 62229 -0.00068 -0.18176 0.16382 -0.000116 0.999999 -0.001532 0.00019 0.00153 1.00000
skipps FlareDiffuseSampler
slingpos 0.09 0.50 0.10
slingrot 70 -30 0
snapresetfix on
snapsync on
ssrjitterpair on
ssrmv off
ssrtileclear on
steadyexact on
steadyview off
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
weaponsmooth 0.20
wheelstick on
xrhalf off
)FC5VR";
}
