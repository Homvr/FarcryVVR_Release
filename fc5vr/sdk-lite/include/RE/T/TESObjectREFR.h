#pragma once
#include "RE/N/NiPoint3.h"
namespace RE
{
  class BGSInventoryList;
  class TESObjectCELL;
	struct OBJ_REFR
	{
	public:
		// members
		NiPoint3                  angle;            // 00
		NiPoint3                  location;         // 0C
		void* objectReference;  // 18 - ref counted in SetObjectReference vfunc
	};
	static_assert(sizeof(OBJ_REFR) == 0x20);


	class TESObjectREFR {
	public:
		virtual ~TESObjectREFR();  // 00

    char pad8[0x70];
    void* valueChangeEvent;  // 78
		// members
		OBJ_REFR                                      data;           // 80
		char inventoryList[16];  // 98
		TESObjectCELL*                                parentCell;     // A8
		char  loadedData[16];     // B0
		void*               extraDataList;  // C0
		std::uint16_t                                 scale;          // C8
		std::uint8_t                                  unkE2;          // CA
		std::uint8_t                                  flags;          // CB

	private:
		void AddLockChange();
	};
	static_assert(sizeof(TESObjectREFR) == 0xD8);
  static_assert(offsetof(TESObjectREFR, data) == 0x80);
}
