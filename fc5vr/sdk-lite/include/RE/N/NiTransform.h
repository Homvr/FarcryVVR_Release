#pragma once

#include "RE/N/NiMatrix3.h"
#include "RE/N/NiPoint3.h"

namespace RE
{
	class NiTransform
	{
	public:
		// members
		NiMatrix3 rotate;         // 00
		NiPoint3  translate;      // 30
		float     scale{ 1.0F };  // 3C

		void MakeIdentity() noexcept
		{
			rotate.MakeIdentity();
			translate = NiPoint3{};
			scale = 1.0F;
		}

		/*inline void copyFrom(const NiTransform* a_rhs)
		{
			if(a_rhs == nullptr) {
				return;
			}
			using func_t = decltype(&NiTransform::copyFrom);
			static REL::Relocation<func_t> func{ ID::NiTransform::copyFrom };
			func(this, a_rhs);
		}*/

		NiTransform operator*(const NiTransform& a_rhs) const
		{
			NiTransform result;
			result.scale = scale * a_rhs.scale;
			result.rotate = rotate * a_rhs.rotate;
			result.translate = translate + (rotate * a_rhs.translate) * scale;
			return result;
		}
/*
        typedef RE::NiTransform* (*NiTransformMulfn)(const RE::NiTransform* parent, RE::NiTransform& out, const RE::NiTransform& local);

        NiTransform operator*(const NiTransform& a_rhs) const
        {
            static auto NiTransformMul = (NiTransformMulfn)((uintptr_t )MemoryScan::mod + 0x127461c);
            NiTransform result;
            NiTransformMul(this, result, a_rhs);
            return result;
        }
*/

		void operator*=(const NiTransform& a_rhs)
		{
			*this = *this * a_rhs;
		}

		NiTransform operator/(const NiTransform& a_rhs) const
		{
			return Inverse() * a_rhs;
		}

		void operator/=(const NiTransform& a_rhs)
		{
			*this = *this / a_rhs;
		}

		NiTransform Inverse() const
		{
			NiTransform result;
			result.scale = scale == 0.0f ? 1.0f : 1.0f / scale;
			result.rotate = rotate.Transpose();
			result.translate = result.rotate * (-translate / result.scale);
			return result;
		}
	};
	static_assert(sizeof(NiTransform) == 0x40);
}
