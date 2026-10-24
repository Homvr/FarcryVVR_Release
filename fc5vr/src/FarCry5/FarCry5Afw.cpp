#include "FarCry5Afw.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <future>
#include <string>
#include <vector>

namespace fc5::afw
{
namespace
{
    // Three passes per rebuild (the Dishonored design, guide sections 6-7):
    //  1-2  seed maps: a grid over each source's depth, carried into the target view and depth-tested,
    //       so every target texel knows the NEAREST source surface there (thin and near things included);
    //       triangles stretched across a depth break are flagged as sheets.
    //  3    compose: each source's seed refined by fixed-point steps through its depth, then the ladder.
    // Depths are metres (R32F), 0 = sky. A source point v (its view space, metres) reaches the target
    // view by M*v + o: the world model (cameras) or, for the foreground, the game-camera model (eye poses).
    const char* kSrc = R"HLSL(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsmain(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); o.uv = uv; return o;
}
Texture2D heldTex : register(t0);
Texture2D heldDepth : register(t1);
Texture2D freshTex : register(t2);
Texture2D freshDepth : register(t3);
Texture2D seedF : register(t4);
Texture2D seedH : register(t5);
SamplerState linSamp : register(s0);
SamplerState pointSamp : register(s1);
cbuffer P : register(b0) {
    float4 fW0, fW1, fW2, fWo;   // fresh view -> target view, world model (rows, offset)
    float4 fG0, fG1, fG2, fGo;   // fresh view -> target view, foreground model
    float4 hW0, hW1, hW2, hWo;   // held, world model
    float4 hG0, hG1, hG2, hGo;   // held, foreground model
    float4 tanF, tanH, tanT;     // tangents (left, right, up, down) of the fresh, held and target views
    float4 prm;                  // target w, h, consistency tolerance (target texels), foreground limit (m)
    float4 prm2;                 // debug tint, stale tolerance (relative), near-miss (texels), foreground by stencil (1) / by depth limit (0)
    float4 prm4;                 // one grid step in uv: fresh x, y, held x, y
    float4 prm5;                 // afwvehnear: in a vehicle, nearer than this (m) only the fresh image (0 = off); y afwvehbody (m)
    float4 fB0, fB1, fB2, fBo;   // afwvehbody: fresh view -> target view for points fixed in the vehicle (its raw frame)
    float4 hB0, hB1, hB2, hBo;   // held, same
};
cbuffer M : register(b1) {
    float4 mp;                   // source (0 fresh, 1 held), grid step (texels), source w, h
    float4 mp2;                  // cells per row, seed w, h, sheet area fraction
};
cbuffer D : register(b2) { float4 dp; float4 dp2; };   // linearise: A, B (metres = B / (z + A)), stencil bits, weapon push (m); the image tangents l r u d. z == 0 is the sky
// afwfpeye: the held eye's own weapon pass (its depth, drawn by the mod with the held eye's camera).
Texture2D<float> fpDepth : register(t6);
cbuffer FP : register(b3) {
    float4 fpOn;                 // on (1), weapon texels per pixel side (1, 2), weapon depth w, h
    float4 fpZ;                  // depth terms A, B (metres = B / (z + A)), push (m), clear value (no weapon)
    float4 fpTan;                // the held weapon pass's tangents (l r u d)
    float4 gF0, gF1, gF2, gFo;   // held view -> fresh view, through the gun's own space (rows, offset)
    float4 gP0, gP1, gP2, gPo;   // held view now -> the held eye's last image's view, same
    float4 fpOk;                 // fresh map ok, held map ok, depth tolerance (m), relative tolerance
    float4 fpOrder;              // afwgunown: x 1 = the held eye's own last image before the fresh one
};

float2 ndcUV(float2 n) { return float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5); }
float3 mr(float4 a, float4 b, float4 c, float3 v) { return float3(dot(a.xyz, v), dot(b.xyz, v), dot(c.xyz, v)); }
float3 viewDir(float2 uv, float4 tn) {
    float2 n = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(tn.x + (n.x + 1.0) * 0.5 * (tn.y - tn.x), tn.w + (n.y + 1.0) * 0.5 * (tn.z - tn.w), -1.0);
}
// Depth maps hold metres, 0 = sky, NEGATIVE metres = the player's weapon and hands (the weapon pass's stencil mark).
// zHr/zFr keep that sign (the class), zH/zF give the distance.
float zHr(float2 uv) { float z = heldDepth.SampleLevel(pointSamp, uv, 0).r; return z != 0.0 ? z : 60000.0; }
float zFr(float2 uv) { float z = freshDepth.SampleLevel(pointSamp, uv, 0).r; return z != 0.0 ? z : 60000.0; }
float zH(float2 uv) { return abs(zHr(uv)); }
float zF(float2 uv) { return abs(zFr(uv)); }
// The foreground (weapon, hands): by the stencil mark when the captures have it, else nearer than the limit.
bool isFg(float zr) { return prm2.w > 0.5 ? zr < 0.0 : abs(zr) < prm.w; }
// afwvehbody: in a vehicle the eyes hang on the vehicle's own frame (its attitude, the seat at the game camera), so the cab
// nearer than prm5.y metres moves by the eye poses in that frame (the foreground model), not as static world.
bool isBody(float zr) { return prm5.y > 0.0 && abs(zr) < prm5.y; }
// A target-view point to (ndc x, ndc y, distance); behind the eye -> far off screen.
float3 toT(float3 L) {
    float zt = -L.z; if (!(zt > 1e-4)) return float3(9, 9, -1);
    float tx = L.x / zt, ty = L.y / zt;
    return float3((2.0 * tx - (tanT.y + tanT.x)) / (tanT.y - tanT.x), (2.0 * ty - (tanT.z + tanT.w)) / (tanT.z - tanT.w), zt);
}
float3 mapF(float2 s, float zr) {
    float3 v = viewDir(s, tanF) * abs(zr);
    return toT(isFg(zr) ? mr(fG0, fG1, fG2, v) + fGo.xyz : isBody(zr) ? mr(fB0, fB1, fB2, v) + fBo.xyz : mr(fW0, fW1, fW2, v) + fWo.xyz);
}
float3 mapH(float2 s, float zr) {
    float3 v = viewDir(s, tanH) * abs(zr);
    return toT(isFg(zr) ? mr(hG0, hG1, hG2, v) + hGo.xyz : isBody(zr) ? mr(hB0, hB1, hB2, v) + hBo.xyz : mr(hW0, hW1, hW2, v) + hWo.xyz);
}
float2 refineF(float2 t, float2 s, out float zt, out float err) {
    float3 m;
    [unroll] for (int i = 0; i < 3; ++i) { m = mapF(s, zFr(s)); s = saturate(s + (t - ndcUV(m.xy))); }
    m = mapF(s, zFr(s)); zt = m.z; err = m.z > 0 ? length((ndcUV(m.xy) - t) * prm.xy) : 1e9; return s;
}
float2 refineH(float2 t, float2 s, out float zt, out float err) {
    float3 m;
    [unroll] for (int i = 0; i < 3; ++i) { m = mapH(s, zHr(s)); s = saturate(s + (t - ndcUV(m.xy))); }
    m = mapH(s, zHr(s)); zt = m.z; err = m.z > 0 ? length((ndcUV(m.xy) - t) * prm.xy) : 1e9; return s;
}
float2 nearF(float2 s) { float2 b = s; float bz = zF(s); float2 o = prm4.xy;
    float2 c[4] = { s + float2(o.x, 0), s - float2(o.x, 0), s + float2(0, o.y), s - float2(0, o.y) };
    [unroll] for (int k = 0; k < 4; ++k) { float z = zF(c[k]); if (z < bz) { bz = z; b = c[k]; } } return b; }
float2 nearH(float2 s) { float2 b = s; float bz = zH(s); float2 o = prm4.zw;
    float2 c[4] = { s + float2(o.x, 0), s - float2(o.x, 0), s + float2(0, o.y), s - float2(0, o.y) };
    [unroll] for (int k = 0; k < 4; ++k) { float z = zH(c[k]); if (z < bz) { bz = z; b = c[k]; } } return b; }
// Debug tint by rule: green fresh foreground, none held world, blue fresh world, red held fallback,
// magenta fill, cyan near-miss.
float4 shade(Texture2D tex, float2 uv, int cls) {
    float3 c = tex.SampleLevel(linSamp, uv, 0).rgb;
    if (prm2.x > 0.5) {
        // afwfpeye: yellow the held eye's own weapon coloured from the fresh image, orange from its last image,
        // purple from a fresh weapon texel nearby.
        float3 k = cls == 0 ? float3(0.5, 1.0, 0.5) : cls == 2 ? float3(0.5, 0.6, 1.0) : cls == 3 ? float3(1.0, 0.4, 0.4)
                 : cls == 5 ? float3(1.0, 0.4, 1.0) : cls == 6 ? float3(0.4, 1.0, 1.0) : cls == 7 ? float3(1.0, 1.0, 0.3)
                 : cls == 8 ? float3(1.0, 0.6, 0.2) : cls == 9 ? float3(0.7, 0.4, 1.0) : float3(1.0, 1.0, 1.0);
        c *= k;
    }
    return float4(c, 1.0);
}
// A true disocclusion: extend the BACKGROUND (the farthest world seed along the row), never the near object.
float4 fill(float2 t) {
    float best = -1.0; float2 bs = t; bool fromF = true;
    [unroll] for (int k = 0; k < 6; ++k) {
        float off = (float)(4 << k) / prm.x;
        [unroll] for (int sd = -1; sd <= 1; sd += 2) {
            float2 p = float2(t.x + sd * off, t.y);
            if (p.x < 0.0 || p.x > 1.0) continue;
            float4 a = seedF.SampleLevel(pointSamp, p, 0); if (a.a > 0 && a.z > best && !isFg(zFr(a.xy))) { best = a.z; bs = a.xy; fromF = true; }
            float4 b = seedH.SampleLevel(pointSamp, p, 0); if (b.a > 0 && b.z > best && !isFg(zHr(b.xy))) { best = b.z; bs = b.xy; fromF = false; }
        }
    }
    return fromF ? shade(freshTex, bs, 5) : shade(heldTex, bs, 5);
}
)HLSL" R"HLSL(
float seedZ(float4 k) { return (k.z < 0.5 ? k.z / 0.5 : (k.z - 0.5) / 0.5); }
// The decision ladder at target texel t. worldOnly (afwfpeye): the weapon and hands of neither source are placed (the
// held eye's own weapon pass draws them), only the world around them.
float4 ladder(float2 t, bool worldOnly) {
    float tol = prm.z;
    float tF, eF, tH, eH;
    float4 kF = seedF.SampleLevel(pointSamp, t, 0);
    float2 sF = refineF(t, kF.a > 0 ? kF.xy : t, tF, eF);
    if (kF.a > 0) { float zs = seedZ(kF); zs = zs / max(1.0 - zs, 1e-5);
        if (kF.a < 0.75 || tF > zs * 1.05 + 0.01) { float ta, ea; float2 sa = refineF(t, nearF(kF.xy), ta, ea);
            if (ea < tol && (!(eF < tol) || ta < tF)) { sF = sa; tF = ta; eF = ea; } } }
    float4 kH = seedH.SampleLevel(pointSamp, t, 0);
    float2 sH = refineH(t, kH.a > 0 ? kH.xy : t, tH, eH);
    if (kH.a > 0) { float zs = seedZ(kH); zs = zs / max(1.0 - zs, 1e-5);
        if (kH.a < 0.75 || tH > zs * 1.05 + 0.01) { float ta, ea; float2 sa = refineH(t, nearH(kH.xy), ta, ea);
            if (ea < tol && (!(eH < tol) || ta < tH)) { sH = sa; tH = ta; eH = ea; } } }
    bool okF = eF < tol, okH = eH < tol;
    bool bF = isFg(zFr(sF)), bH = isFg(zHr(sH));
    // 1. The player's weapon and hands now, from the fresh eye.
    if (!worldOnly && okF && bF) return shade(freshTex, sF, 0);
    // The stale test: the held point, carried to now as static, seen from the fresh view. If the fresh eye
    // sees PAST it (nearest fresh depth around the spot is farther), it moved and gives way.
    bool stale = false;
    if (eH < prm2.z) {
        // target view point -> world-model fresh view: invert the fresh world map (rows are orthonormal).
        float3 Lt = viewDir(t, tanT) * tH;
        float3 q = Lt - fWo.xyz;
        float3 vf = fW0.xyz * q.x + fW1.xyz * q.y + fW2.xyz * q.z;
        float zf = -vf.z;
        if (zf > 1e-4) {
            float2 nf = float2((2.0 * vf.x / zf - (tanF.y + tanF.x)) / (tanF.y - tanF.x), (2.0 * vf.y / zf - (tanF.z + tanF.w)) / (tanF.z - tanF.w));
            float2 uf = ndcUV(nf);
            if (all(uf > 0.0) && all(uf < 1.0)) {
                float2 o = prm4.xy * 0.5;
                float zn = min(min(zF(uf), min(zF(uf + float2(o.x, 0)), zF(uf - float2(o.x, 0)))),
                               min(min(zF(uf + float2(2.0 * o.x, 0)), zF(uf - float2(2.0 * o.x, 0))),
                                   min(zF(uf + float2(0, o.y)), zF(uf - float2(0, o.y)))));
                stale = !isFg(zFr(uf)) && zn > zf * (1.0 + prm2.y) + 0.01;
            }
        }
    }
    // afwvehnear: in a vehicle the cabin and body move with the player, so the held eye's previous image puts them a
    // frame of driving behind (a doubled vehicle): near the eyes only this frame's (fresh) image counts.
    // afwvehbody: the cab within prm5.y was carried with the vehicle, so the held eye's own cab counts (true stereo, and the
    // parts the fresh eye cannot see are not filled with the road); only the farther body (hood) keeps afwvehnear's rule.
    if (prm5.x > 0.0 && tH < prm5.x && !isBody(zHr(sH))) stale = true;
    if (prm5.x > 0.0 && okF && !bF && tF < prm5.x && !isBody(zFr(sF))) return shade(freshTex, sF, 2);
    // 3. The held eye's own world: true stereo, its own shading.
    if (okH && !bH && !stale && !(okF && tF < tH * (1.0 - prm2.y))) return shade(heldTex, sH, 1);
    // 4. Silhouette near-miss: keep the held near-miss over a farther fresh surface (parallax).
    if (!okH && !stale && !bH && eH < prm2.z && okF && tF > tH * (1.0 + prm2.y)) return shade(heldTex, sH, 6);
    // 5. Fresh world: what moved, what the held image could not see.
    if (okF && !(worldOnly && bF)) return shade(freshTex, sF, 2);
    // 6. Held fallback.
    // Never the held eye's own weapon or hands: they are where the hand WAS (the strip beside a near gun that only
    // this eye sees took the old gun from here: a second gun, offset by the gun's disparity, so changing with its distance).
    if (okH && !stale && !(bH && (worldOnly || (prm2.w > 0.5 && prm2.w < 1.5)))) return shade(heldTex, sH, 3);
    // 7. Near-miss beats fill (world only).
    float mF = !bF ? eF : 1e9, mH = (stale || bH) ? 1e9 : eH;
    if (min(mF, mH) < prm2.z) return mF <= mH ? shade(freshTex, sF, 6) : shade(heldTex, sH, 6);
    // 8. Fill.
    return fill(t);
}
)HLSL" R"HLSL(
float2 toUV(float3 v, float4 tn, out float zt) {
    zt = -v.z;
    if (!(zt > 1e-4)) return float2(-1.0, -1.0);
    float tx = v.x / zt, ty = v.y / zt;
    return ndcUV(float2((2.0 * tx - (tn.y + tn.x)) / (tn.y - tn.x), (2.0 * ty - (tn.z + tn.w)) / (tn.z - tn.w)));
}
bool inside(float2 u) { return all(u >= 0.0) && all(u <= 1.0); }
static const float2 kDirs[8] = { float2(1, 0), float2(1, 1), float2(0, 1), float2(-1, 1), float2(-1, 0), float2(-1, -1), float2(0, -1), float2(1, -1) };
// The held eye's weapon at a texel of its weapon depth: the point in the held view (the push taken back), then its colour
// where the fresh eye sees that very point (same instant), else where the held eye's last image had it, else the fresh
// weapon texel nearest in depth around the spot (a part the fresh eye sees behind another part). a = 0: none found.
float4 fpColor(float2 suv, float z) {
    float3 d = viewDir(suv, fpTan);
    float m = max(fpZ.y / (z + fpZ.x) - fpZ.z / sqrt(dot(d.xy, d.xy) + 1.0), 1e-3);
    float3 v = d * m;
    float zt = 0.0, zh = 0.0;
    float2 uf = float2(-1.0, -1.0), uh = float2(-1.0, -1.0);
    bool okF = false, okH = false;
    if (fpOk.x > 0.5) {
        uf = toUV(mr(gF0, gF1, gF2, v) + gFo.xyz, tanF, zt);
        if (inside(uf)) {
            float zr = freshDepth.SampleLevel(pointSamp, uf, 0).r;
            okF = zr < 0.0 && abs(-zr - zt) < fpOk.z + fpOk.w * zt;
        } else uf = float2(-1.0, -1.0);
    }
    if (fpOk.y > 0.5) {
        uh = toUV(mr(gP0, gP1, gP2, v) + gPo.xyz, tanH, zh);
        if (inside(uh)) {
            float zr = heldDepth.SampleLevel(pointSamp, uh, 0).r;
            okH = zr < 0.0 && abs(-zr - zh) < fpOk.z + fpOk.w * zh;
        } else uh = float2(-1.0, -1.0);
    }
    // afwgunown: the held eye's own last image first. Taken from the other eye, the gun's shading (its highlights and
    // reflections, which differ between the eyes) changed every frame in each eye: the surface blinked (Homard 20 Oct).
    if (fpOrder.x > 0.5) {
        if (okH) return shade(heldTex, uh, 8);
        if (okF) return shade(freshTex, uf, 7);
    } else {
        if (okF) return shade(freshTex, uf, 7);
        if (okH) return shade(heldTex, uh, 8);
    }
    // The weapon texel nearest in depth around the spot, in the fresh image and in the held eye's last image (rings of
    // 1, 2, 4, 8, 16 depth texels): the silhouette side only this eye sees, a part hidden behind another one.
    float2 bestF = uf, bestH = uh; float beF = 1e9, beH = 1e9;
    [unroll] for (int r = 0; r <= 4; ++r) {
        float2 o = prm4.xy * 0.5 * (float)(1 << r);
        [unroll] for (int k = 0; k < 8; ++k) {
            if (uf.x >= 0.0) {
                float2 p = uf + kDirs[k] * o;
                float zr = freshDepth.SampleLevel(pointSamp, p, 0).r;
                if (zr < 0.0 && abs(-zr - zt) < beF) { beF = abs(-zr - zt); bestF = p; }
            }
            if (uh.x >= 0.0) {
                float2 p = uh + kDirs[k] * o;
                float zr = heldDepth.SampleLevel(pointSamp, p, 0).r;
                if (zr < 0.0 && abs(-zr - zh) < beH) { beH = abs(-zr - zh); bestH = p; }
            }
        }
    }
    if (beF < 1e8 || beH < 1e8) return beF <= beH ? shade(freshTex, bestF, 9) : shade(heldTex, bestH, 9);
    return float4(0.0, 0.0, 0.0, 0.0);
}
float4 psmain(VSOut i) : SV_Target {
    float2 t = i.uv;
    if (fpOn.x < 0.5) return ladder(t, false);
    // afwfpeye: this pixel's ss x ss texels of the held eye's own weapon depth; the weapon's share of them is blended
    // over the world (the edges smoothed like the fresh eye's, which went through DLSS).
    int ss = (int)fpOn.y;
    float2 cell = floor(t * fpOn.zw / (float)ss) * (float)ss;
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float n = 0.0, got = 0.0;
    [loop] for (int y = 0; y < ss; ++y) {
        [loop] for (int x = 0; x < ss; ++x) {
            float2 px = cell + float2(x, y);
            float z = fpDepth.Load(int3(int2(px), 0));
            if (z != fpZ.w) {
                n += 1.0;
                float4 c = fpColor((px + 0.5) / fpOn.zw, z);
                if (c.a > 0.0) { acc += c; got += 1.0; }
            }
        }
    }
    // Debug tint: red = the held eye's weapon is here but no colour was found for it (it shows the world: a hole).
    if (got == 0.0) return (n > 0.0 && prm2.x > 0.5) ? float4(1.0, 0.0, 0.0, 1.0) : ladder(t, true);
    float4 g = acc / got;
    float cov = n / (float)(ss * ss);
    if (cov > 0.999) return g;
    return lerp(ladder(t, true), g, cov);
}
struct MOut { float4 pos : SV_Position; float3 src : TEXCOORD0; };
MOut vsmesh(uint id : SV_VertexID) {
    uint cell = id / 6, k = id % 6;
    uint gw = (uint)mp2.x;
    uint2 c = uint2(cell % gw, cell / gw);
    uint2 off = k == 0 ? uint2(0, 0) : k == 1 ? uint2(1, 0) : k == 2 ? uint2(0, 1) : k == 3 ? uint2(1, 0) : k == 4 ? uint2(1, 1) : uint2(0, 1);
    float2 px = min(float2(c + off) * mp.y, mp.zw - 1.0) + 0.5;
    float2 s = px / mp.zw;
    float zs = mp.x < 0.5 ? zFr(s) : zHr(s);
    float3 m = mp.x < 0.5 ? mapF(s, zs) : mapH(s, zs);
    MOut o;
    // The foreground wins the depth test, as the game draws it (on top even when inside a wall).
    float dz = m.z / (m.z + 1.0);
    dz = isFg(zs) ? 0.5 * dz : 0.5 + 0.5 * dz;
    o.pos = (m.z > 0 && all(abs(m.xy) < 8.0)) ? float4(m.xy, dz, 1.0) : float4(0, 0, -1, 1);
    o.src = float3(s, m.z);
    return o;
}
float4 psmesh(MOut i) : SV_Target {
    float2 ax = ddx(i.src.xy) * mp.zw, ay = ddy(i.src.xy) * mp.zw;
    float area = abs(ax.x * ay.y - ax.y * ay.x);
    float nominal = (mp.z / mp2.y) * (mp.w / mp2.z);
    float dz = i.src.z / (i.src.z + 1.0);
    return float4(i.src.xy, i.pos.z, area < nominal * mp2.w ? 0.5 : 1.0);
}
// The scene depth (reversed Z) to metres; the sky (0) stays 0.
// dp.z > 0: the stencil bits that mark the weapon pass (hands, gun): those texels get negative metres.
Texture2D<float> sceneDepth : register(t0);
Texture2D<uint2> sceneStencil : register(t1);
float pslinear(VSOut i) : SV_Target {
    float z = sceneDepth.Load(int3(i.pos.xy, 0));
    if (!(z > 0.0)) return 0.0;
    float m = dp.y / (z + dp.x);
    if (dp.z > 0.5 && (sceneStencil.Load(int3(i.pos.xy, 0)).y & (uint)dp.z) != 0) {
        // The weapon pass pushes every vertex dp.w metres farther along its view ray (WorldSpaceZOffset, c52.z: the gun
        // never pokes into walls); the depth is that far too deep. Taken back along this texel's ray (view z = |r| / |dir|).
        float2 sz; sceneDepth.GetDimensions(sz.x, sz.y);
        float2 uv = i.pos.xy / sz;
        float2 d = float2(dp2.x + uv.x * (dp2.y - dp2.x), dp2.w + (1.0 - uv.y) * (dp2.z - dp2.w));
        m = -max(m - dp.w / sqrt(dot(d, d) + 1.0), 1e-3);
    }
    return m;
}
// Flat test: mean absolute colour difference of two images over an 8x8 block.
float4 psdiff(VSOut i) : SV_Target {
    float2 px = i.pos.xy * 8.0; float2 sz; heldTex.GetDimensions(sz.x, sz.y);
    float acc = 0.0;
    [unroll] for (int y = 0; y < 8; y += 2) [unroll] for (int x = 0; x < 8; x += 2) {
        float2 uv = (px + float2(x, y) + 0.5) / sz;
        float3 a = heldTex.SampleLevel(pointSamp, uv, 0).rgb, b = freshTex.SampleLevel(pointSamp, uv, 0).rgb;
        acc += (abs(a.r - b.r) + abs(a.g - b.g) + abs(a.b - b.b)) / 3.0;
    }
    return acc / 16.0;
}
)HLSL";

    template <class T> void release(T*& p)
    {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    struct Tex
    {
        ID3D11Texture2D*          tex{ nullptr };
        ID3D11ShaderResourceView* srv{ nullptr };
        ID3D11RenderTargetView*   rtv{ nullptr };
        uint32_t                  w{ 0 }, h{ 0 };
        DXGI_FORMAT               fmt{ DXGI_FORMAT_UNKNOWN };
        void                      reset()
        {
            release(rtv);
            release(srv);
            release(tex);
            w = h = 0;
            fmt = DXGI_FORMAT_UNKNOWN;
        }
    };

    struct Image
    {
        Tex    color{};
        Tex    depth{};    // R32F metres
        Camera cam{};
        bool   valid{ false };
        bool   stencil{ false };   // the depth carries the weapon pass's stencil class (negative metres)
    };

    struct alignas(16) PCB
    {
        float fW[4][4], fG[4][4], hW[4][4], hG[4][4];
        float tanF[4], tanH[4], tanT[4];
        float prm[4], prm2[4], prm4[4], prm5[4];
        float fB[4][4], hB[4][4];   // afwvehbody: source view -> target view for points fixed in the vehicle
    };
    static_assert(sizeof(PCB) == 31 * 16);

    struct alignas(16) MCB
    {
        float mp[4], mp2[4];
    };

    struct alignas(16) FCB
    {
        float on[4], z[4], tan[4];
        float toF[4][4], toH[4][4];
        float ok[4];
        float order[4];   // afwgunown: x 1 = the held eye's own last image first
    };
    static_assert(sizeof(FCB) == 13 * 16);
    std::atomic<bool> g_gun_own{ true };   // afwgunown (Homard 20 Oct: "in AFW ... the surface is shimmering/blinking")

    ID3D11Device*                 g_dev{ nullptr };
    ID3DDeviceContextState*       g_state{ nullptr };
    ID3D11VertexShader*           g_vs{ nullptr };
    ID3D11VertexShader*           g_vsmesh{ nullptr };
    ID3D11PixelShader*            g_ps{ nullptr };
    ID3D11PixelShader*            g_psmesh{ nullptr };
    ID3D11PixelShader*            g_pslinear{ nullptr };
    ID3D11PixelShader*            g_psdiff{ nullptr };
    ID3D11Buffer*                 g_pcb{ nullptr };
    ID3D11Buffer*                 g_mcb{ nullptr };
    ID3D11Buffer*                 g_dcb{ nullptr };
    ID3D11Buffer*                 g_fcb{ nullptr };     // afwfpeye
    FpInput                       g_fp{};
    ID3D11SamplerState*           g_lin{ nullptr };
    ID3D11SamplerState*           g_point{ nullptr };
    ID3D11DepthStencilState*      g_dss_less{ nullptr };
    ID3D11DepthStencilState*      g_dss_off{ nullptr };
    ID3D11RasterizerState*        g_rs{ nullptr };
    ID3D11BlendState*             g_bs{ nullptr };

    Image                         g_img[2]{};
    Tex                           g_seed[2]{};          // 0 fresh, 1 held
    ID3D11Texture2D*              g_seed_depth{ nullptr };
    ID3D11DepthStencilView*       g_seed_dsv{ nullptr };
    Tex                           g_out{};
    Tex                           g_scene_copy{};       // the game's depth, when it cannot be read directly
    ID3D11ShaderResourceView*     g_scene_srv{ nullptr };
    ID3D11Texture2D*              g_scene_srv_of{ nullptr };

    // Flat test measurement
    Tex                           g_diff{};
    ID3D11Texture2D*              g_diff_staging{ nullptr };
    Tex                           g_pending_rebuild{};  // the last rebuild, kept for comparison
    Tex                           g_pending_held{};     // the held image it was built from (control)
    int                           g_pending_eye{ -1 };

    bool                          g_ready{ false };
    bool                          g_failed{ false };
    std::atomic<bool>             g_enabled{ false };
    std::atomic<bool>             g_measure{ false };
    std::atomic<bool>             g_tint{ false };
    std::atomic<float>            g_fg_limit{ 0.6f };
    // afwfgstencil: the stencil bits the weapon pass writes (ref 64, every bit; the world pass writes only 0x0c), so the
    // gun and hands are the foreground wherever they are; 0 = the old depth limit (a gun at arm's length, 0.6-0.7 m,
    // counted as world and the held eye kept its own old copy of it: the doubled weapon).
    std::atomic<int>              g_fg_stencil{ 0x40 };
    std::atomic<float>            g_fg_push{ 0.0f };     // afwfgpush: the weapon pass's depth push (m) taken back (0 = not)
    std::atomic<bool>             g_held_gun{ false };   // afwheldgun (research): the old way, the held eye's own gun allowed in the fallback
    std::atomic<float>            g_veh_near{ 0.0f };    // afwvehnear: in a vehicle, nearer than this (m) only the fresh image
    std::atomic<float>            g_veh_body{ 0.0f };    // afwvehbody: in a vehicle, nearer than this (m) carried with the vehicle
    ID3D11ShaderResourceView*     g_stencil_srv{ nullptr };
    ID3D11Texture2D*              g_stencil_srv_of{ nullptr };
    std::atomic<int>              g_dump{ 0 };
    int                           g_dump_seq{ 0 };

    // A colour texture (8-bit or 10-bit UNORM) to a 24-bit BMP, for the flat test's image dumps.
    void save_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &st)) || !st) return;
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const uint32_t w = d.Width, h = d.Height, rowb = (w * 3 + 3) & ~3u;
            std::vector<uint8_t> out(54 + size_t(rowb) * h);
            auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&out[o], &v, 4); };
            out[0] = 'B'; out[1] = 'M'; put32(2, uint32_t(out.size())); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
            out[26] = 1; out[28] = 24; put32(34, uint32_t(rowb) * h);
            for (uint32_t y = 0; y < h; ++y) {
                const auto* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                uint8_t*    dst = &out[54 + size_t(h - 1 - y) * rowb];
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t r = 0, g = 0, b = 0;
                    const uint32_t v = reinterpret_cast<const uint32_t*>(src)[x];
                    if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
                        r = uint8_t((v & 1023) >> 2); g = uint8_t(((v >> 10) & 1023) >> 2); b = uint8_t(((v >> 20) & 1023) >> 2);
                    } else if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                        b = uint8_t(v); g = uint8_t(v >> 8); r = uint8_t(v >> 16);
                    } else {
                        r = uint8_t(v); g = uint8_t(v >> 8); b = uint8_t(v >> 16);
                    }
                    dst[x * 3 + 0] = b; dst[x * 3 + 1] = g; dst[x * 3 + 2] = r;
                }
            }
            ctx->Unmap(st, 0);
            if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
        }
        st->Release();
    }
    Stats                         g_stats{};

    constexpr uint32_t kGridStep = 2;
    constexpr uint32_t kSeedDiv  = 2;

    bool compile(const char* entry, const char* target, ID3DBlob** out)
    {
        using CompileFn  = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        static auto fn   = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
        if (fn == nullptr) {
            spdlog::error("[AFW] no D3DCompile");
            return false;
        }
        ID3DBlob* err = nullptr;
        const HRESULT hr = fn(kSrc, std::strlen(kSrc), "fc5afw", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr)) {
            spdlog::error("[AFW] shader {} failed: {}", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            release(err);
            return false;
        }
        release(err);
        return true;
    }

    // precompile(): the six shaders compiled at once on worker threads while the game boots.
    struct Compiled
    {
        ID3DBlob* blob{ nullptr };
        double    ms{ 0.0 };
    };
    constexpr const char* kShaders[6][2] = { { "vsmain", "vs_5_0" }, { "vsmesh", "vs_5_0" }, { "psmain", "ps_5_0" },
                                             { "psmesh", "ps_5_0" }, { "pslinear", "ps_5_0" }, { "psdiff", "ps_5_0" } };
    std::future<Compiled>             g_pre[6];
    std::atomic<bool>                 g_pre_started{ false };
    Compiled compile_timed(const char* entry, const char* target)
    {
        const auto t0 = std::chrono::steady_clock::now();
        Compiled c{};
        if (!compile(entry, target, &c.blob)) c.blob = nullptr;
        c.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return c;
    }

    bool make_tex(Tex& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool rtv)
    {
        if (t.tex && t.w == w && t.h == h && t.fmt == fmt) {
            return true;
        }
        t.reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = fmt;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t.tex)) || FAILED(g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv)) ||
            (rtv && FAILED(g_dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv)))) {
            spdlog::error("[AFW] texture {}x{} format {} refused", w, h, (int)fmt);
            t.reset();
            return false;
        }
        t.w   = w;
        t.h   = h;
        t.fmt = fmt;
        return true;
    }

    DXGI_FORMAT depth_srv_format(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    DXGI_FORMAT typeless_of(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        default: return f;
        }
    }

    // An SRV on the game's depth, directly when it allows one, else on a per-present copy.
    ID3D11ShaderResourceView* scene_depth_srv(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth)
    {
        D3D11_TEXTURE2D_DESC d{};
        depth->GetDesc(&d);
        const auto srv_fmt = depth_srv_format(d.Format);
        if (srv_fmt == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1) {
            return nullptr;
        }
        if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            if (g_scene_srv_of != depth) {
                release(g_scene_srv);
                g_scene_srv_of = nullptr;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format              = srv_fmt;
                sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                if (FAILED(g_dev->CreateShaderResourceView(depth, &sd, &g_scene_srv))) {
                    return nullptr;
                }
                g_scene_srv_of = depth;
                spdlog::info("[AFW] scene depth read directly: {}x{} format {}", d.Width, d.Height, (int)d.Format);
            }
            return g_scene_srv;
        }
        const auto tl = typeless_of(d.Format);
        if (!g_scene_copy.tex || g_scene_copy.w != d.Width || g_scene_copy.h != d.Height || g_scene_copy.fmt != tl) {
            g_scene_copy.reset();
            D3D11_TEXTURE2D_DESC c = d;
            c.Format               = tl;
            c.BindFlags            = D3D11_BIND_SHADER_RESOURCE;
            c.MiscFlags            = 0;
            c.CPUAccessFlags       = 0;
            c.Usage                = D3D11_USAGE_DEFAULT;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format              = srv_fmt;
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (FAILED(g_dev->CreateTexture2D(&c, nullptr, &g_scene_copy.tex)) || FAILED(g_dev->CreateShaderResourceView(g_scene_copy.tex, &sd, &g_scene_copy.srv))) {
                g_scene_copy.reset();
                return nullptr;
            }
            g_scene_copy.w   = d.Width;
            g_scene_copy.h   = d.Height;
            g_scene_copy.fmt = tl;
            spdlog::info("[AFW] scene depth read through a copy: {}x{} format {}", d.Width, d.Height, (int)d.Format);
        }
        ctx->CopyResource(g_scene_copy.tex, depth);
        return g_scene_copy.srv;
    }

    // An SRV on the stencil of the game's depth (read directly only); nullptr when it has none.
    ID3D11ShaderResourceView* scene_stencil_srv(ID3D11Texture2D* depth)
    {
        D3D11_TEXTURE2D_DESC d{};
        depth->GetDesc(&d);
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        if (d.Format == DXGI_FORMAT_R32G8X24_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) f = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if (d.Format == DXGI_FORMAT_R24G8_TYPELESS || d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) f = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        if (f == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
        if (g_stencil_srv_of != depth) {
            release(g_stencil_srv);
            g_stencil_srv_of = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format              = f;
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (FAILED(g_dev->CreateShaderResourceView(depth, &sd, &g_stencil_srv))) return nullptr;
            g_stencil_srv_of = depth;
            spdlog::info("[AFW] weapon and hands told by the scene stencil (bits {:02x})", g_fg_stencil.load());
        }
        return g_stencil_srv;
    }

    void rows_to(float dst[4][4], const glm::mat3& m, const glm::vec3& o)
    {
        for (int r = 0; r < 3; ++r) {
            const glm::vec3 row = glm::row(m, r);
            dst[r][0]           = row.x;
            dst[r][1]           = row.y;
            dst[r][2]           = row.z;
            dst[r][3]           = 0.0f;
        }
        dst[3][0] = o.x;
        dst[3][1] = o.y;
        dst[3][2] = o.z;
        dst[3][3] = 0.0f;
    }

    // Source view -> target view through the world cameras: v_t = Rt (Rs^T v + Ps - Pt).
    void world_map(const Camera& s, const Camera& t, float dst[4][4])
    {
        rows_to(dst, t.rot * glm::transpose(s.rot), t.rot * (s.pos - t.pos));
    }

    // Source view -> target view for content fixed to the game camera (the player's weapon and arms):
    // v_t = rel_t^-1 rel_s v.
    void fg_map(const Camera& s, const Camera& t, float dst[4][4])
    {
        const glm::mat4 m = glm::inverse(t.rel) * s.rel;
        rows_to(dst, glm::mat3{ m }, glm::vec3{ m[3] });
    }

    // afwvehbody: source view -> target view for content fixed in the vehicle (the cab): through the vehicle's raw frame at
    // each image when both have it, else the frame the eyes hang on (the foreground model).
    void body_map(const Camera& s, const Camera& t, float dst[4][4])
    {
        if (!s.veh_valid || !t.veh_valid) {
            fg_map(s, t, dst);
            return;
        }
        // v_t = R_t (V_t^T V_s (R_s^T v + p_s - c_s) + c_t - p_t)
        if (s.veh_mesh && t.veh_mesh) {   // afwvehown: through the car model's own transform (its origin for c)
            const glm::mat3 m = glm::transpose(t.mesh_rot) * s.mesh_rot;
            rows_to(dst, t.rot * m * glm::transpose(s.rot), t.rot * (m * (s.pos - s.mesh_pos) + t.mesh_pos - t.pos));
            return;
        }
        const glm::mat3 carry = glm::transpose(t.veh_rot) * s.veh_rot;
        rows_to(dst, t.rot * carry * glm::transpose(s.rot), t.rot * (carry * (s.pos - s.veh_pos) + t.veh_pos - t.pos));
    }

    void set4(float* d, const glm::vec4& v)
    {
        d[0] = v.x;
        d[1] = v.y;
        d[2] = v.z;
        d[3] = v.w;
    }

    void fullscreen(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11PixelShader* ps)
    {
        D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetDepthStencilState(g_dss_off, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
    }

    // Runs the passes with the mod's own pipeline state and puts the game's back afterwards.
    struct StateScope
    {
        ID3D11DeviceContext1*     ctx1{ nullptr };
        ID3DDeviceContextState*   prev{ nullptr };
        explicit StateScope(ID3D11DeviceContext* ctx)
        {
            if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) {
                ctx1->SwapDeviceContextState(g_state, &prev);
            }
        }
        ~StateScope()
        {
            if (ctx1) {
                ID3D11ShaderResourceView* none[8]{};
                ctx1->PSSetShaderResources(0, 8, none);
                ctx1->VSSetShaderResources(0, 8, none);
                ctx1->OMSetRenderTargets(0, nullptr, nullptr);
                ctx1->SwapDeviceContextState(prev, nullptr);
                release(prev);
                release(ctx1);
            }
        }
    };

    void bind_common(ID3D11DeviceContext* ctx)
    {
        ID3D11SamplerState* s[2]{ g_lin, g_point };
        ctx->PSSetSamplers(0, 2, s);
        ctx->VSSetSamplers(0, 2, s);
        ID3D11Buffer* cbs[3]{ g_pcb, g_mcb, g_dcb };
        ctx->PSSetConstantBuffers(0, 3, cbs);
        ctx->VSSetConstantBuffers(0, 3, cbs);
        ctx->RSSetState(g_rs);
        const float bf[4]{};
        ctx->OMSetBlendState(g_bs, bf, 0xFFFFFFFF);
    }
}

void precompile()
{
    if (g_pre_started.exchange(true)) return;
    for (int i = 0; i < 6; ++i) g_pre[i] = std::async(std::launch::async, compile_timed, kShaders[i][0], kShaders[i][1]);
}

bool initialize(ID3D11Device* device)
{
    if (g_ready || g_failed) {
        return g_ready;
    }
    g_dev = device;
    // The blobs from precompile() (started when the mod loaded); compiled here when it was not started.
    const auto t0 = std::chrono::steady_clock::now();
    const bool pre = g_pre_started.load();
    ID3DBlob* blobs[6]{};
    double    worst_ms = 0.0;
    for (int i = 0; i < 6; ++i) {
        const Compiled c = pre ? g_pre[i].get() : compile_timed(kShaders[i][0], kShaders[i][1]);
        blobs[i]         = c.blob;
        worst_ms         = std::max(worst_ms, c.ms);
    }
    spdlog::info("[AFW] shaders {} (longest compile {:.0f} ms); first frame waited {:.0f} ms for them", pre ? "compiled while the game booted" : "compiled now",
                 worst_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    ID3DBlob *vs = blobs[0], *vsmesh = blobs[1], *ps = blobs[2], *psmesh = blobs[3], *pslin = blobs[4], *psdiff = blobs[5];
    bool ok = vs && vsmesh && ps && psmesh && pslin && psdiff;
    ok = ok && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(device->CreateVertexShader(vsmesh->GetBufferPointer(), vsmesh->GetBufferSize(), nullptr, &g_vsmesh)) &&
         SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps)) &&
         SUCCEEDED(device->CreatePixelShader(psmesh->GetBufferPointer(), psmesh->GetBufferSize(), nullptr, &g_psmesh)) &&
         SUCCEEDED(device->CreatePixelShader(pslin->GetBufferPointer(), pslin->GetBufferSize(), nullptr, &g_pslinear)) &&
         SUCCEEDED(device->CreatePixelShader(psdiff->GetBufferPointer(), psdiff->GetBufferSize(), nullptr, &g_psdiff));
    release(vs);
    release(vsmesh);
    release(ps);
    release(psmesh);
    release(pslin);
    release(psdiff);

    auto make_cb = [&](UINT size, ID3D11Buffer** out) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth      = size;
        d.Usage          = D3D11_USAGE_DYNAMIC;
        d.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&d, nullptr, out));
    };
    ok = ok && make_cb(sizeof(PCB), &g_pcb) && make_cb(sizeof(MCB), &g_mcb) && make_cb(32, &g_dcb) && make_cb(sizeof(FCB), &g_fcb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    ok                = ok && SUCCEEDED(device->CreateSamplerState(&sd, &g_lin));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ok                = ok && SUCCEEDED(device->CreateSamplerState(&sd, &g_point));

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable    = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc      = D3D11_COMPARISON_LESS;
    ok                = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_dss_less));
    dd.DepthEnable    = FALSE;
    ok                = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_dss_off));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode       = D3D11_FILL_SOLID;
    rd.CullMode       = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok                = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &g_rs));
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok                = ok && SUCCEEDED(device->CreateBlendState(&bd, &g_bs));

    ID3D11Device1* dev1 = nullptr;
    if (ok && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
        const D3D_FEATURE_LEVEL lvl = device->GetFeatureLevel();
        ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &lvl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state));
        release(dev1);
    } else {
        ok = false;
    }

    if (!ok) {
        spdlog::error("[AFW] initialisation failed: the held eye stays as it is for this session");
        g_failed = true;
        return false;
    }
    g_ready = true;
    spdlog::info("[AFW] ready (grid step {}, seed maps 1/{}, foreground nearer than {:.2f} m)", kGridStep, kSeedDiv, g_fg_limit.load());
    return true;
}

bool ready() { return g_ready; }

bool capture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* color, ID3D11Texture2D* depth, const Camera& cam, float depth_a, float depth_b)
{
    if (!g_ready || eye < 0 || eye > 1 || color == nullptr) {
        return false;
    }
    auto&                img = g_img[eye];
    D3D11_TEXTURE2D_DESC cd{};
    color->GetDesc(&cd);
    if (!make_tex(img.color, cd.Width, cd.Height, cd.Format, false)) {
        return false;
    }
    ctx->CopyResource(img.color.tex, color);
    img.cam   = cam;
    img.valid = false;

    // Flat test: the previous rebuild of this eye against this native image.
    if (g_measure.load() && g_pending_eye == eye && g_pending_rebuild.tex) {
        if (make_tex(g_diff, (cd.Width + 7) / 8, (cd.Height + 7) / 8, DXGI_FORMAT_R32_FLOAT, true)) {
            if (g_diff_staging == nullptr) {
                D3D11_TEXTURE2D_DESC s{};
                s.Width = g_diff.w; s.Height = g_diff.h; s.MipLevels = 1; s.ArraySize = 1; s.Format = DXGI_FORMAT_R32_FLOAT;
                s.SampleDesc.Count = 1; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                g_dev->CreateTexture2D(&s, nullptr, &g_diff_staging);
            }
            auto measure = [&](Tex& a) -> float {
                StateScope scope{ ctx };
                bind_common(ctx);
                ID3D11ShaderResourceView* srvs[3]{ a.srv, nullptr, img.color.srv };
                ctx->PSSetShaderResources(0, 3, srvs);
                fullscreen(ctx, g_diff.rtv, g_diff.w, g_diff.h, g_psdiff);
                ID3D11ShaderResourceView* none[3]{};
                ctx->PSSetShaderResources(0, 3, none);
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                if (!g_diff_staging) return -1.0f;
                ctx->CopyResource(g_diff_staging, g_diff.tex);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(ctx->Map(g_diff_staging, 0, D3D11_MAP_READ, 0, &m))) return -1.0f;
                double sum = 0.0;
                for (uint32_t y = 0; y < g_diff.h; ++y) {
                    const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
                    for (uint32_t x = 0; x < g_diff.w; ++x) sum += row[x];
                }
                ctx->Unmap(g_diff_staging, 0);
                return float(sum / (double(g_diff.w) * g_diff.h));
            };
            g_stats.last_error          = measure(g_pending_rebuild);
            g_stats.last_error_heldcopy = g_pending_held.tex ? measure(g_pending_held) : -1.0f;
            if (g_dump.load() > 0) {
                g_dump--;
                const std::string base = "D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/dumps/" + std::to_string(g_dump_seq++);
                CreateDirectoryA("D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/dumps", nullptr);
                save_bmp(ctx, g_pending_rebuild.tex, base + "_rebuilt.bmp");
                save_bmp(ctx, img.color.tex, base + "_native.bmp");
                save_bmp(ctx, g_pending_held.tex, base + "_held.bmp");
                spdlog::info("[AFW] dumped {}_*.bmp (eye {}, error rebuilt {:.4f}, held as it is {:.4f})", base, eye, g_stats.last_error, g_stats.last_error_heldcopy);
            }
        }
        g_pending_eye = -1;
    }

    if (depth == nullptr) {
        g_stats.depth_missing++;
        return false;
    }
    D3D11_TEXTURE2D_DESC dd{};
    depth->GetDesc(&dd);
    if (!make_tex(img.depth, dd.Width, dd.Height, DXGI_FORMAT_R32_FLOAT, true)) {
        return false;
    }
    auto* srv = scene_depth_srv(ctx, depth);
    if (srv == nullptr) {
        g_stats.depth_missing++;
        return false;
    }
    const int                 st_bits = g_fg_stencil.load();
    ID3D11ShaderResourceView* ssrv    = st_bits > 0 ? scene_stencil_srv(depth) : nullptr;
    {
        StateScope scope{ ctx };
        bind_common(ctx);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_dcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const float v[8]{ depth_a, depth_b, ssrv ? float(st_bits) : 0.0f, g_fg_push.load(), cam.tan.x, cam.tan.y, cam.tan.z, cam.tan.w };
            std::memcpy(m.pData, v, sizeof(v));
            ctx->Unmap(g_dcb, 0);
        }
        ID3D11ShaderResourceView* srvs[2]{ srv, ssrv };
        ctx->PSSetShaderResources(0, 2, srvs);
        fullscreen(ctx, img.depth.rtv, img.depth.w, img.depth.h, g_pslinear);
    }
    img.stencil = ssrv != nullptr;
    img.valid = true;
    g_stats.captures++;
    return true;
}

ID3D11Texture2D* rebuild(ID3D11DeviceContext* ctx, int held, const Camera& target, const char** why, DXGI_FORMAT out_format)
{
    auto refuse = [&](const char* r) -> ID3D11Texture2D* {
        g_stats.refused++;
        g_stats.last_why = r;
        if (why) *why = r;
        return nullptr;
    };
    if (!g_ready) return refuse("not initialised");
    if (held < 0 || held > 1) return refuse("bad eye");
    auto& H = g_img[held];
    auto& F = g_img[held ^ 1];
    if (!H.valid || !F.valid) return refuse("no image with depth for one of the eyes");
    if (!target.valid || !H.cam.valid || !F.cam.valid) return refuse("no camera");
    if (H.color.w != F.color.w || H.color.h != F.color.h || H.depth.w != F.depth.w) return refuse("eye sizes differ");

    const uint32_t w = H.color.w, h = H.color.h, dw = H.depth.w, dh = H.depth.h;
    if (!make_tex(g_out, w, h, out_format == DXGI_FORMAT_UNKNOWN ? H.color.fmt : out_format, true)) return refuse("no output texture");
    const uint32_t sw = std::max(1u, w / kSeedDiv), sh = std::max(1u, h / kSeedDiv);
    for (auto& s : g_seed) {
        if (!make_tex(s, sw, sh, DXGI_FORMAT_R16G16B16A16_UNORM, true)) return refuse("no seed map");
    }
    if (!g_seed_dsv || [&] { D3D11_TEXTURE2D_DESC d{}; g_seed_depth->GetDesc(&d); return d.Width != sw || d.Height != sh; }()) {
        release(g_seed_dsv);
        release(g_seed_depth);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sw; d.Height = sh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_D32_FLOAT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_seed_depth)) || FAILED(g_dev->CreateDepthStencilView(g_seed_depth, nullptr, &g_seed_dsv))) {
            return refuse("no seed depth");
        }
    }

    PCB p{};
    world_map(F.cam, target, p.fW);
    fg_map(F.cam, target, p.fG);
    world_map(H.cam, target, p.hW);
    fg_map(H.cam, target, p.hG);
    body_map(F.cam, target, p.fB);
    body_map(H.cam, target, p.hB);
    set4(p.tanF, F.cam.tan);
    set4(p.tanH, H.cam.tan);
    set4(p.tanT, target.tan);
    const float depth_texel = std::max(1.0f, float(w) / float(dw));
    set4(p.prm, { float(w), float(h), 1.5f * depth_texel, g_fg_limit.load() });
    set4(p.prm2, { g_tint.load() ? 1.0f : 0.0f, 0.015f, 6.0f, (H.stencil && F.stencil) ? (g_held_gun.load() ? 2.0f : 1.0f) : 0.0f });
    set4(p.prm4, { float(kGridStep) / dw, float(kGridStep) / dh, float(kGridStep) / dw, float(kGridStep) / dh });
    set4(p.prm5, { g_veh_near.load(), g_veh_body.load(), 0.0f, 0.0f });

    StateScope scope{ ctx };
    bind_common(ctx);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_pcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return refuse("constants refused");
    std::memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(g_pcb, 0);

    ID3D11ShaderResourceView* srcs[4]{ H.color.srv, H.depth.srv, F.color.srv, F.depth.srv };
    ctx->VSSetShaderResources(0, 4, srcs);

    // Seed maps
    const uint32_t cells_x = (dw + kGridStep - 1) / kGridStep, cells_y = (dh + kGridStep - 1) / kGridStep;
    for (int s = 0; s < 2; ++s) {
        MCB mc{ { float(s), float(kGridStep), float(dw), float(dh) }, { float(cells_x), float(sw), float(sh), 1.0f / 9.0f } };
        if (FAILED(ctx->Map(g_mcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return refuse("constants refused");
        std::memcpy(m.pData, &mc, sizeof(mc));
        ctx->Unmap(g_mcb, 0);
        const float clear[4]{ 0, 0, 0, 0 };
        ctx->ClearRenderTargetView(g_seed[s].rtv, clear);
        ctx->ClearDepthStencilView(g_seed_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{ 0, 0, float(sw), float(sh), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &g_seed[s].rtv, g_seed_dsv);
        ctx->OMSetDepthStencilState(g_dss_less, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(g_vsmesh, nullptr, 0);
        ctx->PSSetShader(g_psmesh, nullptr, 0);
        ctx->Draw(cells_x * cells_y * 6, 0);
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Compose
    // afwfpeye: the held eye's own weapon pass, when the mod drew it this frame and both images carry the weapon's
    // stencil mark (the fresh eye's weapon is told from the world by it).
    const bool fp = g_fp.srv != nullptr && g_fp.w > 0 && g_fp.h > 0 && H.stencil && F.stencil && p.prm2[3] > 0.5f;
    {
        FCB fc{};
        if (fp) {
            set4(fc.on, { 1.0f, float(std::clamp(g_fp.ss, 1, 2)), float(g_fp.w), float(g_fp.h) });
            set4(fc.z, { g_fp.pa, g_fp.pb, g_fp.push, g_fp.clear });
            set4(fc.tan, g_fp.tan);
            rows_to(fc.toF, g_fp.to_fresh, g_fp.to_fresh_o);
            rows_to(fc.toH, g_fp.to_held, g_fp.to_held_o);
            // A point is the same gun point when its depths agree within 1.5 cm + 3 % (the push is taken back in both).
            set4(fc.ok, { g_fp.fresh_ok ? 1.0f : 0.0f, g_fp.held_ok ? 1.0f : 0.0f, 0.015f, 0.03f });
            set4(fc.order, { g_gun_own.load() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f });   // afwgunown
        }
        if (FAILED(ctx->Map(g_fcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return refuse("constants refused");
        std::memcpy(m.pData, &fc, sizeof(fc));
        ctx->Unmap(g_fcb, 0);
        ctx->PSSetConstantBuffers(3, 1, &g_fcb);
    }
    ID3D11ShaderResourceView* all[7]{ H.color.srv, H.depth.srv, F.color.srv, F.depth.srv, g_seed[0].srv, g_seed[1].srv, fp ? g_fp.srv : nullptr };
    ctx->PSSetShaderResources(0, 7, all);
    fullscreen(ctx, g_out.rtv, w, h, g_ps);

    g_stats.rebuilds++;
    if (fp) g_stats.fp_rebuilds++;
    if (why) *why = "";

    if (g_measure.load()) {
        if (g_out.fmt == H.color.fmt && make_tex(g_pending_rebuild, w, h, H.color.fmt, false) && make_tex(g_pending_held, w, h, H.color.fmt, false)) {
            ctx->CopyResource(g_pending_rebuild.tex, g_out.tex);
            ctx->CopyResource(g_pending_held.tex, H.color.tex);
            g_pending_eye = held;
        }
    }
    return g_out.tex;
}

bool copy_last(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* dst)
{
    if (eye < 0 || eye > 1 || !g_img[eye].color.tex || dst == nullptr) {
        return false;
    }
    ctx->CopyResource(dst, g_img[eye].color.tex);
    return true;
}

ID3D11Texture2D* last_image(int eye) { return (eye == 0 || eye == 1) && g_img[eye].valid ? g_img[eye].color.tex : nullptr; }
const Camera&    last_camera(int eye) { return g_img[eye & 1].cam; }
void             set_measure(bool on) { g_measure = on; }
void             request_dump(int n) { g_dump = n; }
void             set_debug_tint(bool on) { g_tint = on; }
void             set_gun_own(bool on) { g_gun_own = on; }
bool             gun_own() { return g_gun_own.load(); }
bool             debug_tint() { return g_tint.load(); }
void             set_foreground_limit(float metres) { g_fg_limit = metres; }
float            foreground_limit() { return g_fg_limit.load(); }
void             set_foreground_stencil(int bits) { g_fg_stencil = bits; }
void             set_foreground_push(float metres) { g_fg_push = metres; }
int              foreground_stencil() { return g_fg_stencil.load(); }
void             set_held_gun(bool on) { g_held_gun = on; }
void             set_fp_input(const FpInput& in) { g_fp = in; }
void             set_vehicle_near(float metres) { g_veh_near = metres; }
void             set_vehicle_body(float metres) { g_veh_body = metres; }
void             set_enabled(bool on) { g_enabled = on; }
bool             enabled() { return g_enabled.load(); }
Stats&           stats() { return g_stats; }
void             save_image(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path)
{
    if (!g_dev) {
        tex->GetDevice(&g_dev);
        if (g_dev) g_dev->Release();   // the game's device outlives the mod
    }
    save_bmp(ctx, tex, path);
}

void shutdown()
{
    for (auto& i : g_img) {
        i.color.reset();
        i.depth.reset();
    }
    for (auto& s : g_seed) s.reset();
    g_out.reset();
    g_scene_copy.reset();
    g_diff.reset();
    g_pending_rebuild.reset();
    g_pending_held.reset();
    release(g_diff_staging);
    release(g_scene_srv);
    release(g_stencil_srv);
    g_stencil_srv_of = nullptr;
    release(g_seed_dsv);
    release(g_seed_depth);
    release(g_state);
    g_ready = false;
}
}

// ---------------------------------------------------------------------------------------------------------
// Stereo pair check (fc5::pair): see FarCry5Afw.h.
#include <mutex>
#include <thread>
namespace fc5::pair
{
    namespace
    {
        constexpr UINT kW = 384;
        const char* kShrink = R"(
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
RWTexture2DArray<float> dst : register(u0);
cbuffer C : register(b0) { uint slice; uint W; uint H; uint pad; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    float2 px = 1.0 / float2(W, H);
    float2 uv = (float2(id.xy) + 0.5) * px;
    float3 c = src.SampleLevel(lin, uv + float2(-0.25, -0.25) * px, 0).rgb + src.SampleLevel(lin, uv + float2(0.25, -0.25) * px, 0).rgb
             + src.SampleLevel(lin, uv + float2(-0.25, 0.25) * px, 0).rgb + src.SampleLevel(lin, uv + float2(0.25, 0.25) * px, 0).rgb;
    dst[uint3(id.xy, slice)] = dot(saturate(c * 0.25), float3(0.299, 0.587, 0.114));
}
)";
        ID3D11Device*              g_d{ nullptr };
        ID3D11ComputeShader*       g_cs{ nullptr };
        ID3D11SamplerState*        g_lin{ nullptr };
        ID3D11Buffer*              g_cb{ nullptr };
        ID3D11Texture2D*           g_luma{ nullptr };
        ID3D11UnorderedAccessView* g_uav{ nullptr };
        UINT                       g_h{ 0 };
        bool                       g_tried{ false };
        struct Pending { ID3D11Texture2D* tex{}; bool busy{}; int frame{}; float tan[2][4]{}; };
        Pending  g_ring[3]{};
        int      g_ring_i{ 0 };
        bool     g_have[2]{};
        int      g_frame[2]{ -10, -10 };
        float    g_tan[2][4]{};
        std::atomic<bool> g_worker{ false };
        std::mutex        g_res_mtx;
        Result            g_res{};
        std::vector<float> g_prev[2];

        DXGI_FORMAT typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }

        bool init(ID3D11DeviceContext* ctx, UINT w, UINT h)
        {
            const UINT nh = std::max<UINT>(32, (UINT)std::lround(double(kW) * h / std::max<UINT>(1, w)));
            if (g_luma && nh == g_h) return true;
            if (!g_d) ctx->GetDevice(&g_d);
            if (!g_cs && !g_tried) {
                g_tried = true;
                using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
                auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
                ID3DBlob* code = nullptr;
                ID3DBlob* err  = nullptr;
                if (compile_fn && SUCCEEDED(compile_fn(kShrink, strlen(kShrink), "fc5_pair_shrink", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                    g_d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
                    code->Release();
                } else {
                    spdlog::error("[Pair] shrink shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no compiler");
                }
                if (err) err->Release();
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                g_d->CreateSamplerState(&sd, &g_lin);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                g_d->CreateBuffer(&bd, nullptr, &g_cb);
            }
            if (!g_cs || !g_lin || !g_cb) return false;
            if (g_uav) { g_uav->Release(); g_uav = nullptr; }
            if (g_luma) { g_luma->Release(); g_luma = nullptr; }
            for (auto& p : g_ring) { if (p.tex) p.tex->Release(); p = {}; }
            g_h = nh;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kW; td.Height = nh; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(g_d->CreateTexture2D(&td, nullptr, &g_luma))) return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_FLOAT; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY; ud.Texture2DArray.ArraySize = 2;
            if (FAILED(g_d->CreateUnorderedAccessView(g_luma, &ud, &g_uav))) return false;
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& p : g_ring) g_d->CreateTexture2D(&td, nullptr, &p.tex);
            return true;
        }

        float pct(std::vector<float> v, float q)
        {
            if (v.empty()) return 0.0f;
            const size_t k = std::min(v.size() - 1, (size_t)std::lround(q * (v.size() - 1)));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        }

        // Block matching of the left image's textured 16 px tiles in the right one, around where a point at
        // infinity would land (each eye's own view tangents), then disparity in milliradians of tangent.
        void analyze(std::vector<float> L, std::vector<float> R, int H, int frame, float tl[4], float tr[4])
        {
            Result res{};
            res.valid = true;
            res.frame = frame;
            const int W = (int)kW;
            double s = 0;
            for (size_t i = 0; i < L.size(); ++i) s += std::abs(L[i] - R[i]);
            res.mean_abs = float(s / std::max<size_t>(1, L.size()));
            std::vector<float>* cur[2]{ &L, &R };
            for (int e = 0; e < 2; ++e) {
                if (g_prev[e].size() == cur[e]->size()) {
                    double c = 0;
                    for (size_t i = 0; i < cur[e]->size(); ++i) c += std::abs((*cur[e])[i] - g_prev[e][i]);
                    res.change[e] = float(c / cur[e]->size());
                }
                g_prev[e] = *cur[e];
            }
            auto tanx = [&](const float* t, float x) { return t[0] + (x + 0.5f) / W * (t[1] - t[0]); };
            auto tany = [&](const float* t, float y) { return t[2] + (y + 0.5f) / H * (t[3] - t[2]); };
            constexpr int T = 16, SX = 28, SY = 5;
            std::vector<float> disp, vert;
            int negative = 0;
            for (int y0 = SY + 2; y0 + T + SY + 2 < H; y0 += T) {
                for (int x0 = 4; x0 + T < W - 4; x0 += T) {
                    double m = 0, m2 = 0;
                    for (int y = 0; y < T; ++y) for (int x = 0; x < T; ++x) { const float v = L[(y0 + y) * W + x0 + x]; m += v; m2 += v * v; }
                    m /= T * T;
                    if (m2 / (T * T) - m * m < 0.0004) continue;   // flat tile
                    const float cx = tanx(tl, x0 + T * 0.5f - 0.5f), cy = tany(tl, y0 + T * 0.5f - 0.5f);
                    const int   px = (int)std::lround((cx - tr[0]) / (tr[1] - tr[0]) * W - 0.5f - T * 0.5f + 0.5f);
                    const int   py = (int)std::lround((cy - tr[2]) / (tr[3] - tr[2]) * H - 0.5f - T * 0.5f + 0.5f);
                    double best = 1e30, second = 1e30;
                    int    bx = 0, by = 0;
                    std::vector<double> row(2 * SX + 1, 1e30);
                    for (int dy = -SY; dy <= SY; ++dy) {
                        for (int dx = -SX; dx <= SX; ++dx) {
                            const int rx = px + dx, ry = py + dy;
                            if (rx < 0 || ry < 0 || rx + T > W || ry + T > H) continue;
                            double sad = 0;
                            for (int y = 0; y < T; ++y) {
                                const float* a = &L[(y0 + y) * W + x0];
                                const float* b = &R[(ry + y) * W + rx];
                                for (int x = 0; x < T; ++x) sad += std::abs(a[x] - b[x]);
                            }
                            if (sad < best) { if (std::abs(dx - bx) > 2 || std::abs(dy - by) > 1) second = best; best = sad; bx = dx; by = dy; }
                            else if (sad < second && (std::abs(dx - bx) > 2 || std::abs(dy - by) > 1)) second = sad;
                        }
                    }
                    if (best >= 1e29 || best > 0.85 * second) continue;   // ambiguous
                    // Sub-pixel along x: parabola through the neighbours.
                    float sub = 0.0f;
                    auto sad_at = [&](int dx) {
                        const int rx = px + dx, ry = py + by;
                        if (rx < 0 || rx + T > W) return 1e30;
                        double sad = 0;
                        for (int y = 0; y < T; ++y) for (int x = 0; x < T; ++x) sad += std::abs(L[(y0 + y) * W + x0 + x] - R[(ry + y) * W + rx + x]);
                        return sad;
                    };
                    const double a = sad_at(bx - 1), c = sad_at(bx + 1);
                    if (a < 1e29 && c < 1e29 && a + c - 2 * best > 1e-9) sub = float(0.5 * (a - c) / (a + c - 2 * best));
                    const float mx = tanx(tr, px + bx + sub + T * 0.5f - 0.5f), my = tany(tr, py + by + T * 0.5f - 0.5f);
                    const float d  = (cx - mx) * 1000.0f, v = (cy - my) * 1000.0f;
                    disp.push_back(d);
                    vert.push_back(v);
                    if (d < -2.0f) negative++;
                }
            }
            res.tiles = (int)disp.size();
            if (!disp.empty()) {
                res.disp_med = pct(disp, 0.5f); res.disp_p10 = pct(disp, 0.1f); res.disp_p90 = pct(disp, 0.9f);
                res.neg_frac = float(negative) / disp.size();
                res.vert_med = pct(vert, 0.5f);
                std::vector<float> av(vert.size());
                for (size_t i = 0; i < vert.size(); ++i) av[i] = std::abs(vert[i]);
                res.vert_p90 = pct(av, 0.9f);
            }
            if (res.mean_abs < 0.002f) res.verdict = "identical";
            else if (res.tiles < 20) res.verdict = "flat";
            else if (res.disp_med < -1.0f || res.neg_frac > 0.3f) res.verdict = "swapped?";
            else if (std::abs(res.vert_med) > 2.0f || res.vert_p90 > 6.0f) res.verdict = "vertical";
            else res.verdict = "ok";
            std::lock_guard _{ g_res_mtx };
            g_res = res;
        }
    }

    void capture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* image, const float tan[4], int frame)
    {
        if (!image || eye < 0 || eye > 1) return;
        D3D11_TEXTURE2D_DESC sd{};
        image->GetDesc(&sd);
        if (!init(ctx, sd.Width, sd.Height)) return;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = typed(sd.Format);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(g_d->CreateShaderResourceView(image, &vd, &srv)) || !srv) return;
        ID3D11ComputeShader*       prev_cs = nullptr;
        ID3D11ShaderResourceView*  prev_srv = nullptr;
        ID3D11UnorderedAccessView* prev_uav = nullptr;
        ID3D11Buffer*              prev_cb = nullptr;
        ID3D11SamplerState*        prev_ss = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &prev_srv);
        ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
        ctx->CSGetConstantBuffers(0, 1, &prev_cb);
        ctx->CSGetSamplers(0, 1, &prev_ss);
        const UINT cb[4]{ (UINT)eye, kW, g_h, 0 };
        ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);
        ctx->CSSetSamplers(0, 1, &g_lin);
        ctx->Dispatch((kW + 7) / 8, (g_h + 7) / 8, 1);
        ctx->CSSetShader(prev_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &prev_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &prev_cb);
        ctx->CSSetSamplers(0, 1, &prev_ss);
        for (auto* p : { (IUnknown*)prev_cs, (IUnknown*)prev_srv, (IUnknown*)prev_uav, (IUnknown*)prev_cb, (IUnknown*)prev_ss }) if (p) p->Release();
        srv->Release();
        g_have[eye]  = true;
        g_frame[eye] = frame;
        for (int k = 0; k < 4; ++k) g_tan[eye][k] = tan[k];
        if (g_have[0] && g_have[1] && std::abs(g_frame[0] - g_frame[1]) <= 1) {
            auto& p = g_ring[g_ring_i];
            if (!p.busy && p.tex) {
                ctx->CopyResource(p.tex, g_luma);
                p.busy  = true;
                p.frame = std::min(g_frame[0], g_frame[1]);
                std::memcpy(p.tan, g_tan, sizeof g_tan);
                g_ring_i = (g_ring_i + 1) % 3;
            }
            g_have[0] = g_have[1] = false;
        }
    }

    void poll(ID3D11DeviceContext* ctx)
    {
        for (auto& p : g_ring) {
            if (!p.busy || !p.tex || g_worker.load()) continue;
            std::vector<float> img[2];
            bool ok = true;
            for (UINT s = 0; s < 2 && ok; ++s) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (ctx->Map(p.tex, D3D11CalcSubresource(0, s, 1), D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) { ok = false; break; }
                img[s].resize(size_t(kW) * g_h);
                for (UINT y = 0; y < g_h; ++y) std::memcpy(&img[s][size_t(y) * kW], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, kW * 4);
                ctx->Unmap(p.tex, D3D11CalcSubresource(0, s, 1));
            }
            if (!ok) continue;
            p.busy = false;
            g_worker = true;
            float tl[4], tr[4];
            std::memcpy(tl, p.tan[0], sizeof tl);
            std::memcpy(tr, p.tan[1], sizeof tr);
            std::thread([L = std::move(img[0]), R = std::move(img[1]), H = (int)g_h, f = p.frame, tl, tr]() mutable {
                float a[4], b[4];
                std::memcpy(a, tl, sizeof a);
                std::memcpy(b, tr, sizeof b);
                analyze(std::move(L), std::move(R), H, f, a, b);
                g_worker = false;
            }).detach();
            break;
        }
    }

    Result last()
    {
        std::lock_guard _{ g_res_mtx };
        return g_res;
    }
}
