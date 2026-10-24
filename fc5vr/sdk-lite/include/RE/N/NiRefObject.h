#pragma once

namespace RE
{
	class alignas(16) NiRefObject
	{
	public:
//		SF_RTTI_VTABLE(NiRefObject);

		virtual ~NiRefObject();  // 00

		// add
		virtual void DeleteThis();  // 01

		void                                  IncRefCount(){};
		void                                  DecRefCount(){};

		// members
		volatile std::uint32_t _refCount{ 0 };  // 08
		std::uint32_t          _msUiObjects{ 0 };  // 0C
	};
	static_assert(sizeof(NiRefObject) == 0x10);
}
