#pragma once

namespace RE
{
    class NiNode;
    class NiCamera;
    class BSSceneNode;
    class NiAVObject;

    class Main
    {
    public:
        struct GlobalWorldScene
        {
            NiNode*      pNinode[11];
            uint32_t     atmosphericScatteringId;
            uint32_t     imageSpaceHandlerId;
            uint32_t     dirtOverlayStorageKey;
            BSSceneNode* pBSSceneNode;
        };
        static_assert(sizeof(GlobalWorldScene) == 0x70);
        static_assert(offsetof(GlobalWorldScene, atmosphericScatteringId) == 0x58);
        static_assert(offsetof(GlobalWorldScene, pBSSceneNode) == 0x68);

        struct MainSceneStarfieldSceneResources
        {
            uint8_t  field_0;
            int      unkStorageTableKeyId;
            float    field_8;
            int      field_C;
            int      field_10;
            float    field_14;
            float    field_18;
            float    field_1C;
            int      field_20;
            int      field_24;
            int      field_28;
            int      field_2C;
            int      field_30;
            int      field_34;
            int      field_38;
            int      field_3C;
            int      field_40;
            int      field_44;
            int      field_48;
            int      field_4C;
            int      field_50;
            int      field_54;
            int      field_58;
            int      field_5C;
            int      field_60;
            int      field_64;
            int      field_68;
            int      field_6C;
            int      field_70;
            int      field_74;
            int      field_78;
            int      field_7C;
            int      field_80;
            uint16_t field_84;
            float    field_88[4];
            int      field_98;
            int      field_9C;
            int      field_A0;
            int      field_A4;
            float    field_A8[4];
            int      field_B8;
            int      field_BC;
            int      field_C0;
            char     field_C4;
            float    field_C8;
            float    field_CC;
            int      field_D0;
            int      field_D4;
        };
        static_assert(sizeof(MainSceneStarfieldSceneResources) == 0xD8);

#pragma pack(push, 1)
        struct MainSceneGraphStarfieldScene
        {
            BSSceneNode*                     pSceneNode;
            NiAVObject*                      pShadowLightNode;
            NiAVObject*                      pShadowLightGalaxySceneDirectionalLight;
            NiCamera*                        pGalaxyCamera;
            NiCamera*                        pStarFieldCamera;
            int                              field_28;
            uint8_t                          gap2C[36];
            int                              field_50;
            uint8_t                          gap54[52];
            NiNode*                          pNiNode88;
            NiNode*                          pNiNode90;
            int                              field_98;
            uint8_t                          gap9C[52];
            int                              field_D0;
            uint16_t                         field_D4;
            uint16_t                         field_D6;
            int                              field_D8;
            int                              field_DC;
            int                              field_E0;
            uint8_t                          gapE4[268];
            int                              field_1F0;
            int                              field_1F4;
            int                              field_1F8;
            int                              field_1FC;
            uint64_t                         field_200;
            uint64_t                         field_208;
            uint64_t                         field_210;
            uint64_t                         field_218;
            int                              field_220;
            uint32_t                         pad224;
            uint32_t                         unkStorageTableId;
            uint32_t                         starfeidldataSceneId;
            uint32_t                         planetDataStorageId;
            MainSceneStarfieldSceneResources sceneResources;
            uint8_t                          gap238[36];
            uint64_t                         field_330;
        };
#pragma pack(pop)
        static_assert(sizeof(MainSceneGraphStarfieldScene) == 0x338);

#pragma pack(push, 1)
        struct SceneGraphRoot
        {
            NiCamera*                    pFullscreenCamera;
            GlobalWorldScene             globalScene;
            NiNode*                      worldCameraRoot;
            NiCamera*                    worldCamera;
            uint64_t                     qword88;
            MainSceneGraphStarfieldScene starfieldScene;
            uint64_t                     pad3C8;
            uint32_t                     sceneViewHandleID;
            uint32_t                     sceneViewHandleID2;
            uint32_t                     renderHandleID;
            uint32_t                     renderHandleID2; // renderer for menus
            uint8_t                      trailing[120];
            uint32_t                     currentRendererID;
            uint8_t                      trailing1[124];
            uint32_t                     currentRendererID2;
            uint8_t                      trailing2[5];
            uint8_t                      isInStarfieldView;
            uint8_t                      trailing3[14];
        };
#pragma pack(pop)
        static_assert(offsetof(SceneGraphRoot, worldCamera) == 0x80);
        static_assert(offsetof(SceneGraphRoot, sceneViewHandleID) == 0x3D0);

        /*inline static SceneGraphRoot* WorldRoot()
        {
            static REL::Relocation<SceneGraphRoot**> singleton{ REL::ID(887308) };
            return *singleton;
        }

        inline static NiCamera* WorldCamera()
        {
            return WorldRoot()->worldCamera;
        }*/
    };
}
