#pragma once

namespace RE
{
	class NiPoint3
	{
	public:
		constexpr NiPoint3() noexcept = default;

		constexpr NiPoint3(float a_x, float a_y, float a_z) noexcept :
			x(a_x),
			y(a_y),
			z(a_z){};

		float&       operator[](std::size_t a_idx);
		const float& operator[](std::size_t a_idx) const;
		bool         operator==(const NiPoint3& a_rhs) const;
		bool         operator!=(const NiPoint3& a_rhs) const;
        inline NiPoint3 operator+(const NiPoint3& a_rhs) const
        {
            return NiPoint3(x + a_rhs.x, y + a_rhs.y, z + a_rhs.z);
        }
		NiPoint3     operator-(const NiPoint3& a_rhs) const
        {
            return NiPoint3(x - a_rhs.x, y - a_rhs.y, z - a_rhs.z);
        }
		float        operator*(const NiPoint3& a_rhs) const;
		inline NiPoint3     operator*(float a_scalar) const noexcept
        {
            return NiPoint3(x * a_scalar, y * a_scalar, z * a_scalar);
        }
		NiPoint3     operator/(float a_scalar) const
        {
            return NiPoint3(x / a_scalar, y / a_scalar, z / a_scalar);
        }
		NiPoint3     operator-() const {
            return NiPoint3(-x, -y, -z);
        }
		NiPoint3&    operator+=(const NiPoint3& a_rhs);
		NiPoint3&    operator-=(const NiPoint3& a_rhs);
		NiPoint3&    operator*=(const NiPoint3& a_rhs);
		NiPoint3&    operator/=(const NiPoint3& a_rhs);
		NiPoint3&    operator*=(float a_scalar);
		NiPoint3&    operator/=(float a_scalar);

		[[nodiscard]] NiPoint3 Cross(const NiPoint3& pt) const;
		[[nodiscard]] float    Dot(const NiPoint3& pt) const;
		[[nodiscard]] float    GetDistance(const NiPoint3& a_pt) const noexcept;
		[[nodiscard]] float    GetSquaredDistance(const NiPoint3& a_pt) const noexcept;
		[[nodiscard]] float    Length() const;
		[[nodiscard]] float    SqrLength() const;
		[[nodiscard]] NiPoint3 UnitCross(const NiPoint3& a_pt) const;
		float                  Unitize();

		// members
		float x{ 0.0f };  // 0x0
		float y{ 0.0f };  // 0x4
		float z{ 0.0f };  // 0x8
	};
	static_assert(sizeof(NiPoint3) == 0xC);

	class alignas(0x10) NiPoint3A :
		public NiPoint3
	{
	public:
		char padding[4];  // 0xC
	};
	static_assert(sizeof(NiPoint3A) == 0x10);
}
