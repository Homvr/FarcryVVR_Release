#pragma once

namespace RE::Scaleform
{
	class RefCountImplCore
	{
	public:

		RefCountImplCore() = default;

		virtual ~RefCountImplCore() = default;  // 00

	protected:
		// members
		volatile std::int32_t refCount{ 1 };  // 08
	};
	static_assert(sizeof(RefCountImplCore) == 0x10);

	class RefCountImpl :
		public RefCountImplCore  // 00
	{
	public:

		virtual ~RefCountImpl() override = default;  // 00
	};
	static_assert(sizeof(RefCountImpl) == 0x10);

	template <class Base, std::int32_t>
	class RefCountBaseStatImpl :
		public Base  // 00
	{
	public:
		RefCountBaseStatImpl() = default;

		virtual ~RefCountBaseStatImpl() = default;  // 00
	};

	template <class T, std::int32_t STAT>
	class RefCountBase :
		public RefCountBaseStatImpl<RefCountImpl, STAT>  // 00
	{
	public:
		RefCountBase() = default;

		virtual ~RefCountBase() = default;  // 00
	};
}
