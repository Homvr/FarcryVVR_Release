#pragma once
#include <cassert>

namespace RE
{
	class __declspec(novtable) Setting
	{
	public:

		enum class Type
		{
			kNone = 0,
			kBool,
			kChar,
			kUChar,
			kInt,
			kUInt,
			kFloat,
			kString,
			kRGB,
			kRGBA
		};

		virtual ~Setting() = default;

		// add
		[[nodiscard]] virtual bool IsPrefSetting() { return false; }  // 01

		[[nodiscard]] auto GetKey() const noexcept
		{
			return _key ? _key : std::string_view{ "unk"};
		}

		[[nodiscard]] auto GetType() const noexcept
		{
			if (_key) {
				switch (_key[0]) {
				case 'a':
					return Type::kRGBA;
				case 'b':
					return Type::kBool;
				case 'c':
					return Type::kChar;
				case 'f':
					return Type::kFloat;
				case 'h':
					return Type::kUChar;
				case 'i':
					return Type::kInt;
				case 'r':
					return Type::kRGB;
				case 's':
					return Type::kString;  // static
				case 'S':
					return Type::kString;  // dynamic
				case 'u':
					return Type::kUInt;
				}
			}

			return Type::kNone;
		}

    template <class T>
    [[nodiscard]] bool Is() const noexcept;

		[[nodiscard]] auto GetBool(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kBool);
			return a_default ? _defaultValue.b : _value.b;
		}

		[[nodiscard]] auto GetChar(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kChar);
			return a_default ? _defaultValue.c : _value.c;
		}

		[[nodiscard]] auto GetUChar(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kUChar);
			return a_default ? _defaultValue.h : _value.h;
		}

		[[nodiscard]] auto GetFloat(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kFloat);
			return a_default ? _defaultValue.f : _value.f;
		}

		[[nodiscard]] auto GetInt(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kInt);
			return a_default ? _defaultValue.i : _value.i;
		}

		[[nodiscard]] auto GetUInt(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kUInt  || GetType() == Type::kRGB || GetType() == Type::kRGBA);
			return a_default ? _defaultValue.u : _value.u;
		}

		[[nodiscard]] auto GetString(const bool a_default = false) const noexcept
		{
			assert(GetType() == Type::kString);
			if (!a_default && _value.s)
				return std::string_view{ _value.s };
			else if (_defaultValue.s)
				return std::string_view{ _defaultValue.s };

			return std::string_view{ "unk"};
		}

		template <class T>
		[[nodiscard]] T GetValue(T a_fallback, const bool a_default = false) const noexcept
		{
			if (Is<T>()) {
				if constexpr (std::is_same_v<T, bool>)
					return GetBool(a_default);
				else if constexpr (std::is_same_v<T, std::int8_t>)
					return GetChar(a_default);
				else if constexpr (std::is_same_v<T, std::uint8_t>)
					return GetUChar(a_default);
				else if constexpr (std::is_same_v<T, std::int32_t>)
					return GetInt(a_default);
				else if constexpr (std::is_same_v<T, std::uint32_t>)
					return GetUInt(a_default);
				else if constexpr (std::is_same_v<T, float>)
					return GetFloat(a_default);
				else if constexpr (std::is_convertible_v<T, std::string_view>)
					return GetString(a_default);
			}

			return a_fallback;
		}

		void SetBool(const bool a_value) noexcept
		{
			assert(GetType() == Type::kBool);
			_value.b = a_value;
		}

		void SetChar(const std::int8_t a_value) noexcept
		{
			assert(GetType() == Type::kChar);
			_value.c = a_value;
		}

		void SetUChar(const std::uint8_t a_value) noexcept
		{
			assert(GetType() == Type::kUChar);
			_value.h = a_value;
		}

		void SetInt(const std::int32_t a_value) noexcept
		{
			assert(GetType() == Type::kInt);
			_value.i = a_value;
		}

		void SetUInt(const std::uint32_t a_value) noexcept
		{
			assert(GetType() == Type::kUInt || GetType() == Type::kRGB || GetType() == Type::kRGBA);
			_value.u = a_value;
		}

		void SetFloat(const float a_value) noexcept
		{
			assert(GetType() == Type::kFloat);
			_value.f = a_value;
		}

		void SetString(char* a_value) noexcept
		{
			assert(GetType() == Type::kString);
			_value.s = a_value;
		}

		void SetString(const char* a_value) noexcept
		{
			assert(GetType() == Type::kString);
			_value.s = _strdup(a_value);
		}

		template <class T>
		bool SetValue(T a_value) noexcept
		{
			if (Is<T>()) {
				if constexpr (std::is_same_v<T, bool>)
					SetBool(a_value);
				else if constexpr (std::is_same_v<T, std::int8_t>)
					SetChar(a_value);
				else if constexpr (std::is_same_v<T, std::uint8_t>)
					SetUChar(a_value);
				else if constexpr (std::is_same_v<T, std::int32_t>)
					SetInt(a_value);
				else if constexpr (std::is_same_v<T, std::uint32_t>)
					SetUInt(a_value);
				else if constexpr (std::is_same_v<T, float>)
					SetFloat(a_value);
				else if constexpr (std::is_convertible_v<T, std::string_view>)
					SetString(a_value);

				return true;
			}

			return false;
		}

		Setting& operator=(const bool a_value)
		{
			SetBool(a_value);
			return *this;
		}

		Setting& operator=(const std::int8_t a_value)
		{
			SetChar(a_value);
			return *this;
		}

		Setting& operator=(const std::uint8_t a_value)
		{
			SetUChar(a_value);
			return *this;
		}

		Setting& operator=(const std::int32_t a_value)
		{
			SetInt(a_value);
			return *this;
		}

		Setting& operator=(const std::uint32_t a_value)
		{
			SetUInt(a_value);
			return *this;
		}

		Setting& operator=(const float a_value)
		{
			SetFloat(a_value);
			return *this;
		}

		Setting& operator=(char* a_value)
		{
			SetString(a_value);
			return *this;
		}

		Setting& operator=(const char* a_value)
		{
			SetString(a_value);
			return *this;
		}

		inline std::string to_string() const
		{
			return FormatSettingValue(this);
		}

		inline static std::string FormatSettingValue(const Setting* setting)
		{
			switch (setting->GetType())
			{
			case Setting::Type::kBool:
			{
				bool value = setting->GetBool();
				return value ? "true" : "false";
			}
			case Setting::Type::kChar:
			{
				char value = setting->GetChar();
				return std::string(1, value);
			}
			case Setting::Type::kUChar:
			{
				unsigned char value = setting->GetUChar();
				return std::to_string(value);
			}
			case Setting::Type::kInt:
			{
				int value = setting->GetInt();
				return std::to_string(value);
			}
			case Setting::Type::kUInt:
			{
				unsigned int value = setting->GetUInt();
				return std::to_string(value);
			}
			case Setting::Type::kFloat:
			{
				float value = setting->GetFloat();
				return std::to_string(value);
			}
			case Setting::Type::kString:
			{
				std::string_view value = setting->GetString();
				return std::string(value);
			}
			case Setting::Type::kRGB:
			{
				unsigned int value = setting->GetUInt();
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "#%06X", value & 0xFFFFFF);
				return std::string(buffer);
			}
			case Setting::Type::kRGBA:
			{
				unsigned int value = setting->GetUInt();
				char buffer[10];
				std::snprintf(buffer, sizeof(buffer), "#%08X", value);
				return std::string(buffer);
			}
			default:
				return "";
			}
		}

	private:
		union Value
		{
			std::uint32_t a;
			bool          b;
			std::int8_t   c;
			float         f;
			std::uint8_t  h;
			std::int32_t  i;
			std::uint32_t r;
			char*         s;
			std::uint32_t u;
		};
		static_assert(sizeof(Value) == 0x8);

		// members
		Value       _value;         // 08
		Value       _defaultValue;  // 10
		const char* _key;           // 18
	};
	static_assert(sizeof(Setting) == 0x20);

	template <class T>
	class SettingT :
		public Setting
	{
	public:
      ~SettingT() override = default;

		static REL::Relocation<T*> collection;
	};

	class GameSettingCollection;
	class INIPrefSettingCollection;
	class INISettingCollection;
	class RegSettingCollection;

	extern template class SettingT<GameSettingCollection>;
	extern template class SettingT<INIPrefSettingCollection>;
	extern template class SettingT<INISettingCollection>;
	extern template class SettingT<RegSettingCollection>;
} // namespace RE

namespace RE
{
    template <class T>
    [[nodiscard]] bool Setting::Is() const noexcept
    {
        if constexpr (std::is_same_v<T, bool>)
            return GetType() == Type::kBool;
        else if constexpr (std::is_same_v<T, std::int8_t>)
            return GetType() == Type::kChar;
        else if constexpr (std::is_same_v<T, std::uint8_t>)
            return GetType() == Type::kUChar;
        else if constexpr (std::is_same_v<T, std::int32_t>)
            return GetType() == Type::kInt;
        else if constexpr (std::is_same_v<T, std::uint32_t>)
            return GetType() == Type::kUInt;
        else if constexpr (std::is_same_v<T, float>)
            return GetType() == Type::kFloat;
        else if constexpr (std::is_convertible_v<T, std::string_view>)
            return GetType() == Type::kString;
        else
            return false;
    }
} // namespace RE
