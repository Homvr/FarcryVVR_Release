#pragma once
#include "RE/B/BSFixedString.h"
#include "RE/N/NiTransform.h"
#include "RE/N/NiBound.h"
#include "RE/N/NiRefObject.h"

namespace RE
{
	class NiNode;
	class BSLight;
	class NiAVObject;
	class NiCamera;
	class NiObject;
	class NiExtraData;

	#pragma pack(push, 1)
	struct NiCloneProcess
	{
		uint64_t  unk0{0};
		uint64_t  unk8{0};
		uint64_t  unk10{0};
		uint64_t  unk18{0};
		uint32_t  dword_7FF744EB3960{1};
		uint16_t  flag{0x2400};
		uint8_t   flag26{1};
		uint8_t  unk27;
		float     unk_28{1.0f};
		float     unk_2C{1.0f};
		float    unk_30{1.0f};
	};
	#pragma pack(pop)

	template <class T>
	struct BSArray
	{
		uint32_t size;
		uint32_t capacity;
		T*       data;
	};

	template <class T>
	class NiTArray
	{
	public:
		virtual ~NiTArray() = default;
		T*       entries;
		uint16_t m_arrayBufLen;    // 10 - max elements storable in m_data
		uint16_t m_emptyRunStart;  // 12 - index of beginning of empty slot run
		uint16_t m_size;           // 14 - number of filled slots
		uint16_t m_growSize;       // 16 - number of slots to grow m_data by
	};

	class NiRTTI
	{
	public:
		const char* name;
		const char* parent;
	};

	struct NiUpdateData
	{
		float     delta = 0;
		float     unk04 = 0;
		NiCamera* camera = nullptr;
		NiPoint4* unk10 = nullptr;
		NiPoint4* unk18 = nullptr;
		uint64_t  unk20 = 0; // memset 0,36 here looks like allockation for 36 bytes
		uint64_t  unk28 = 0;
		uint64_t  unk30 = 0; // unk34 float possible deltaY/X rotation
		uint32_t  flags = 0x10;
		uint32_t  flags2 = 1;
		uint32_t  flags3 = 1;
		uint32_t  frameCounter = 0; // timer or counter or frame counter get's from offset 0x59AE8A0
		float     unk48 = 0; // possible delta X/Y rotation
		uint32_t  unk4C = 0;
		float     unk50 = 0; // unk50 is rotation matrix
		float     unk54 = 0;
		float     unk58 = 0;
		uint32_t  unk5C = 0;
	};

	class NiObject: public NiRefObject
	{
	public:
		virtual ~NiObject() = default;
		virtual NiRTTI*     GetRTTI();
		virtual NiNode*     GetAsNiNode();
		virtual NiNode*     GetAsNiSwitchNode();
		virtual void*       Unk5() { return nullptr; }
		virtual void*       Unk6() { return nullptr; }
		virtual void*       Unk7() { return nullptr; }
		virtual void*       Unk8() { return nullptr; }
		virtual NiAVObject* GetAsBSGeometry() { return nullptr; }
		virtual void*       Unk10() { return nullptr; }
		virtual void*       Unk11() { return nullptr; }
		virtual void*       Unk12() { return nullptr; }
		virtual void*       Unk13() { return nullptr; }
		virtual void*       Unk14() { return nullptr; }
		virtual void*       Unk15() { return nullptr; }
		virtual void*       Unk16() { return nullptr; }
		virtual void*       Unk17() { return nullptr; }
		virtual void*       Unk18() { return nullptr; }
		virtual void*       Unk19() { return nullptr; }
		virtual void*       Unk20() { return nullptr; }
		virtual void*       Unk21() { return nullptr; }
		virtual void*       Unk22() { return nullptr; }
		virtual void*       Unk23() { return nullptr; }
		virtual void*       Unk24() { return nullptr; }
		virtual void*       Unk25() { return nullptr; }
		virtual void*       Unk26() { return nullptr; }
		virtual void*       Unk27() { return nullptr; }
		virtual void*       Unk28() { return nullptr; }
		virtual void*       Unk29() { return nullptr; }
		virtual void*       Unk30() { return nullptr; }
		virtual BSLight*    GetAsBSLight() { return nullptr; }
		virtual void*       Unk32() { return nullptr; }
		virtual void*       Unk33() { return nullptr; }
		virtual void*       Unk34() { return nullptr; }
		virtual void*       Unk35() { return nullptr; }
		virtual void*       Unk36() { return nullptr; }
		virtual void*       Unk37() { return nullptr; }
		virtual void*       Unk38() { return nullptr; }
		virtual void*       Unk39() { return nullptr; }
		virtual void*       Unk40() { return nullptr; } // all castings end here
		virtual void*       createClone(NiCloneProcess* cloningProcess);
		virtual void*       streaming1(); // some sort of streaming 2
		virtual void*       Unk43();
		virtual void*       Unk44();
		virtual void*       streaming2();  // some sort of streaming
		virtual bool        equals(NiObject* second);
		virtual void*       Unk47();
		virtual void*       Unk48();
		virtual void*       Unk49();
		virtual void*       getRTTIDelegate();  // calls GetRTTI internally
		virtual void*       Unk51(); 
		virtual void*       Unk52();
		virtual void*       Unk53();
		virtual void*       Unk54();
	};

	class NiAVObject : public NiObject
	{
	public:
		virtual ~NiAVObject() = default;
		virtual void*   Update4(NiUpdateData* data); // calls update3 or 2 function (they are all point to same address)
		virtual void*   setAsCameraRoot(void* parent); // calls *0x14598A710->tvable[1](NiAVObject)
		virtual void*   Unk57();
		virtual void*   Unk58();
		virtual void*   Unk59();
		virtual void*   Unk60();
		virtual void*   SetSelectiveFlagsAndPropagadeToParent(bool set); // flags | 1 and much more  
		virtual NiNode* GetObjectByName(const BSFixedString& name);
		virtual void*   SetSelectiveUpdateFlags(); // +execute something on each controller->*0x1D8
		virtual void*   update0x800FlagsAndResetWorldBoundRadius(bool set);         // v2 | 0x800; if set is true and worldBound.radius = 1.0 if it's negative
		virtual void*   update0x10000000000Flags(bool set);  // a1->flags | 0x10000000000i64;
		virtual void*   update0x100000000Flags(bool set);    // a1->flags | 0x100000000i64;
		virtual void*   execUnk66or72OnCondition(); // result & 0x100000000i64) != 0  will execute update0x100000000Flags else Unk72 with 1 and clear flag
		virtual void*   execUnk66or72(); // (a1->flags & 0x200000000i64) == 0 will execute update0x100000000Flags else Unk72 with 0 and clear flag
		virtual void*   update0x100Flags(bool set);    //flags | 0x100;
		virtual void*   update0x20000Flags(bool set);  // flags | 0x20000 if set is true
		virtual void*   update0x80000Flags(bool set); // flags | 0x80000 is set is true
		virtual void*   Unk72();
		virtual void*   Update(NiUpdateData* dta); // pass update to all controllers and call world update
		virtual void*   Update2(NiUpdateData* data); // pass update to all controllers and call unk78 with update
		virtual void*   Update3(NiUpdateData* data); // pass update to all controllers and call unk78 with update
		virtual void*   Unk76();
		virtual void*   Unk77();
		virtual void*   Unk78(); // noop function not implemented in nicamera as well
		virtual void*   UpdateWorldData(NiUpdateData* data);
		virtual void*   UpdateTransformAndBounds(NiUpdateData* data);  // calls UpdateWorldData if flags & 4 == 0 also calls unk78 with 0 also flips flags if they are in specific state
		virtual void*   UpdateTransforms(NiUpdateData* data); // calls UpdateWorldData if flags & 4 != 0
		virtual void*   Unk82();
		virtual void*   Unk83();

		BSFixedString   name;        // 10
		void*           controller;  // 18
		void*           unk28;       // 20
		void*           unk30;       // 28
		void*           unk38;       // 30 BSArray<NiExtraData*>*
		NiNode*       parent;         //38
		NiTransform   local;          //40
		NiTransform   world;          //80
		NiTransform   previousWorld;  //C0
		NiBound       worldBound;     //100
		NiObject*         collisionObject; // NiCollisionObject* 110
		uint64_t      flags;   //118
		void*         unk120;  // 120
		uint32_t             lastUpdatedFrame;  // 128
		uint32_t             pad12C;  // 128
	};
	static_assert(sizeof(NiAVObject) == 0x130);
	static_assert(offsetof(NiAVObject, parent) == 0x38);
	static_assert(offsetof(NiAVObject, local) == 0x40);

	class NiExtraData : public NiObject
	{
	public:
		BSFixedString	m_kName;	// 10
	};

	class BSTransformExtra : public NiExtraData
	{
	public:
		NiTransform m_transform;	// 20
	};
	static_assert(offsetof(BSTransformExtra, m_transform) == 0x20);
	static_assert(sizeof(BSTransformExtra) == 0x60);


  class NiNode : public NiAVObject
  {
  public:
      virtual ~NiNode() = default;
      virtual void* AttachChild(NiAVObject* child, bool firstAvailable);
      virtual void* InsertChildAt(uint32_t idx, NiAVObject* child);
      virtual void* DetachChild(NiAVObject* child);
      virtual void* DetachChildOut(NiAVObject* child, NiAVObject** outObj);  //RE::NiPointer<NiAVObject>&
      virtual void* DetachChildAt(uint32_t idx);
      virtual void* DetachChildAtOut(uint32_t idx, NiAVObject** outObj);  //RE::NiPointer<NiAVObject>&
      virtual void* SetAt(uint32_t idx, NiAVObject* child);
      virtual void* SetAtOut(uint32_t idx, NiAVObject* child, NiAVObject** outObj);  //RE::NiPointer<NiAVObject>&
      virtual void* Unk92();
      virtual void* Unk93();
      virtual void* Unk94();

      NiTArray<NiNode*> children;  //NiTObjectArray<NiPointer<NiAVObject>> //130
      void*             unk148;
  };

}
