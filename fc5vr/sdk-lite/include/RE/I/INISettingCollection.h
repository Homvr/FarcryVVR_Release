#pragma once

#include "RE/S/Setting.h"
#include "RE/S/SettingCollectionList.h"
#include "REL/Relocation.h"
#include <CreationEngine/memory/offsets.h>

namespace RE
{
	class INISettingCollection :
		public SettingCollectionList<Setting>  // 000
	{
	public:
		virtual ~INISettingCollection();  // 000

//		[[nodiscard]] static INISettingCollection* GetSingleton()
//		{
//			REL::Relocation<INISettingCollection**> singleton{ GameStore::MemoryOffsets::Setting::INISettingCollection() };
//			return *singleton;
//		}

		[[nodiscard]] Setting* GetSetting(const std::string_view a_name)
		{
			for (const auto& setting : settings) {
				if (setting->GetKey() == a_name) {
					return setting;
				}
			}

			return nullptr;
		}

		template <class T>
		T GetSetting(const std::string_view a_name, T a_fallback, const bool a_default = false)
		{
			if (const auto setting = GetSetting(a_name)) {
				return setting->GetValue(a_fallback, a_default);
			}

			return a_fallback;
		}

		template <class T>
		bool SetSetting(const std::string_view a_name, T a_value)
		{
			if (const auto setting = GetSetting(a_name)) {
				return setting->SetValue(a_value);
			}

			return false;
		}
	};
}
