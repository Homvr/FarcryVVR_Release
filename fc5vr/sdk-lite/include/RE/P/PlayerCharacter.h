#pragma once
#include <RE/T/TESObjectREFR.h>
#include "RE/E/EquippedWeapon.h"

namespace RE {

    enum class BOOL_FLAGS {
        kIsInScene = 0x8,
    };

    struct TESForm {
        char pad[0x28];
        int pad28;
        uint8_t flags[4]; // 0x2E == 48 formTypeWeapon
    };
    static_assert(offsetof(TESForm, flags) == 0x2C);

    struct EquippedItem {
        TESForm* weaponInstanceData;
        char pad[0x18];
        EquippedWeapon::Data* equippedWeapon; // 0x20
    };

    /*
     struct RE::EquippedItem
{
RE::TESObjectWEAP *weaponInstance;
RE::TESObjectWEAPInstanceData *weaponInstanceData;
RE::BGSEquipSlot *equipSlot;
char pad[8];
RE::EquippedWeapon::Data *equippedWeapon;
};

     */

    static_assert(sizeof(EquippedItem) == 0x28);
    static_assert(offsetof(EquippedItem, equippedWeapon) == 0x20);

    struct EquippedItems {
        int size;
        int capacity;
        EquippedItem* items;
    };
    static_assert(sizeof(EquippedItems) == 0x10);

    struct MiddleHighProcessData {
        char pad0x0[0x270];
        EquippedItems equippedItems; // 270
        char pad0x278[0x298];
        uint32_t isInIronSights; // 08
    };
    static_assert(offsetof(MiddleHighProcessData, isInIronSights) == 0x518);

    class AIProcess {
    public:
        char pad0x0[0x8];
        MiddleHighProcessData*          middleHigh;            // 08
    };

    class PlayerCharacter : public TESObjectREFR
    {
    public:
        char pad0x80[0x20];
        uint32_t                                       actorState;             // 0F8
        uint32_t actorState2; // 0fC
        char pad0x90[0x108];
        uint32_t boolFlags; // 0x208
        uint32_t unk; // 0x20C
        char pad0x210[0x18];
        AIProcess*                                       currentProcess;           // 228
        uint8_t pad0x230[0xCB0];
        NiAVObject* crosshairHit; // EE0


        // not really sure how this works
        inline bool IsInScene() const noexcept { return (boolFlags & static_cast<uint32_t>(BOOL_FLAGS::kIsInScene)) != 0; }
        inline bool IsInIronSights() const noexcept {
            if(!currentProcess || !currentProcess->middleHigh) return false;
            return currentProcess->middleHigh->isInIronSights;
        }
        inline bool IsWeaponDrawn() const noexcept { return (actorState2 & 7) >= 3; }
        // probably for zero gravity not for ship
        inline bool IsInFlyingState() const noexcept { return (actorState >> 14) & 7; }
        inline bool Immovable() const noexcept { return (actorState2 >> 12) & 1; }
        inline bool ControlledByAI() const noexcept { return (actorState2 >> 11) & 1; }

        inline NiNode* getEquippedWeaponProjectile() const noexcept {
            if(!currentProcess || !currentProcess->middleHigh) return nullptr;
            auto equippedItem = currentProcess->middleHigh->equippedItems.items;
            if(!equippedItem) return nullptr;
            for(int i = 0; i < currentProcess->middleHigh->equippedItems.size; i++) {
                if(equippedItem[i].weaponInstanceData->flags[2] == 48) {
                    return equippedItem[i].equippedWeapon->visualModule->projectileNode;
                }
            }
            return nullptr;
        }

        inline NiNode* getEquippedWeaponLaserSight() const noexcept {
            if(!currentProcess || !currentProcess->middleHigh) return nullptr;
            auto equippedItem = currentProcess->middleHigh->equippedItems.items;
            if(!equippedItem) return nullptr;
            for(int i = 0; i < currentProcess->middleHigh->equippedItems.size; i++) {
                if(equippedItem[i].weaponInstanceData->flags[2] == 48) {
                    return equippedItem[i].equippedWeapon->visualModule->laserNode;
                }
            }
            return nullptr;
        }

        inline NiNode* getLaserNode(int deep) {
            auto equippedWeaponNode = getEquippedWeaponLaserSight();
            if(!equippedWeaponNode) return nullptr;
            for(int i = 0; i < deep; i++) {
                equippedWeaponNode = equippedWeaponNode->parent;
                if(!equippedWeaponNode) return nullptr;
            }
            return equippedWeaponNode;
        }

        inline NiNode* getProjectileNode(int deep) {
            auto equippedWeaponNode = getEquippedWeaponProjectile();
            if(!equippedWeaponNode) return nullptr;
            for(int i = 0; i < deep; i++) {
                equippedWeaponNode = equippedWeaponNode->parent;
                if(!equippedWeaponNode) return nullptr;
            }
            return equippedWeaponNode;
        }

        inline std::pair<NiNode*, NiNode*> getEquippedWeaponRootNode() const noexcept {
            auto equippedWeaponNode = getEquippedWeaponProjectile();
            if(!equippedWeaponNode) return {nullptr, nullptr};
            NiNode* previousNode = nullptr;
            while (equippedWeaponNode) {
                if(strcmp(equippedWeaponNode->name.data(), "Weapon") == 0) {
                    return {equippedWeaponNode, previousNode};
                }
                previousNode = equippedWeaponNode;
                equippedWeaponNode = equippedWeaponNode->parent;
            }
            return {nullptr, nullptr};
        }

    };

    static_assert(offsetof(PlayerCharacter, crosshairHit) == 0xEE0);
    static_assert(offsetof(PlayerCharacter, actorState) == 0xF8);
    static_assert(offsetof(PlayerCharacter, actorState2) == 0xFC);
    static_assert(offsetof(PlayerCharacter, boolFlags) == 0x208);
    static_assert(offsetof(PlayerCharacter, currentProcess) == 0x228);
}