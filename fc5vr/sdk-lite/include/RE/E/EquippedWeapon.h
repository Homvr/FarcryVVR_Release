#pragma once
#include "RE/N/NiAVObject.h"

namespace RE {
    struct WeaponDataAim {
        char pad[0x69];
        bool aimFromMuzzle;
    };
    struct TESObjectWEAPInstanceData {
        char pad[0x18];
        WeaponDataAim* aimData;  // 18
    };

    struct AimOpticalSightData {
        char pad[0x28];
        BSFixedString  nodeName;                     // 28
        float          beamReactivationDelayReload;  // 30
        float          beamDeactivationDelayReload;  // 34
        float          beamReactivationDelayFiring;  // 38
        float          unk3C;                        // 3C
        char           unk40;                        // 40
    };

    struct OpticalAimTemplate {
        char pad[96];
        AimOpticalSightData *opticalSightsData;
    };
}

namespace RE::EquippedWeapon
{
  struct VisualModule {
    char pad[0x40];
    NiNode* laserNode;  // 40
    char pad0x48[0x88];
    NiNode* projectileNode;  // D0
  };

  struct AimModule {
    void* vtbl;
    struct Data* data;  // 8
    void* pad10;
    OpticalAimTemplate *aim_template;
  };

  struct Data
  {
    char pad[0x18];
    TESObjectWEAPInstanceData* instanceData;  // 18
    AimModule* aimModule;  // 20
    char pad0x28[0x30];
    VisualModule* visualModule;  // 58

    inline bool IsAimFromMuzzle() {
        if(!instanceData || !instanceData->aimData)
            return false;
        return instanceData->aimData->aimFromMuzzle;
    }

    inline bool IsSomething() {
        if(!aimModule || !aimModule->aim_template || !aimModule->aim_template->opticalSightsData) {
            return false;
        }
        spdlog::info("IsSomething {} {}", aimModule->aim_template->opticalSightsData->nodeName, aimModule->aim_template->opticalSightsData->unk40);
        return aimModule->aim_template->opticalSightsData->unk40 || aimModule->aim_template->opticalSightsData->nodeName.length() == 0;
    }
  };
  static_assert(offsetof(Data, visualModule) == 0x58);
}
