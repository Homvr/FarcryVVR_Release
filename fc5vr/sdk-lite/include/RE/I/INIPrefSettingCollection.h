#pragma once

#include "RE/I/INISettingCollection.h"
#include "CreationEngine/memory/offsets.h"

namespace RE
{
	class INIPrefSettingCollection :
		public INISettingCollection  // 000
	{
	public:
		virtual ~INIPrefSettingCollection();  // 000

		[[nodiscard]] static INIPrefSettingCollection* GetSingleton()
		{
			REL::Relocation<INIPrefSettingCollection**> singleton{ GameStore::MemoryOffsets::Setting::INIPrefSettingCollection() };
			return *singleton;
		}
	};
}
