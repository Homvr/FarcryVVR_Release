#pragma once

#include "RE/N/NiPoint4.h"
#include "RE/N/NiPoint3.h"
#include <REL/Relocation.h>
#include <glm/ext/scalar_constants.hpp>
#include <CreationEngine/memory/ScanHelper.h>

namespace RE
{
	static float HALF_PI = glm::pi<float>() / 2.0f;
	class alignas(0x10) NiMatrix3
	{
	public:
		void MakeIdentity() noexcept
		{
			entry[0].v = { 1.0F, 0.0F, 0.0F, 0.0F };
			entry[1].v = { 0.0F, 1.0F, 0.0F, 0.0F };
			entry[2].v = { 0.0F, 0.0F, 1.0F, 0.0F };
		}

		NiMatrix3 operator*(const NiMatrix3& rhs) const
		{
			NiMatrix3 tmp;
			tmp.entry[0].pt[0] =
				entry[0].pt[0] * rhs.entry[0].pt[0] +
				entry[0].pt[1] * rhs.entry[1].pt[0] +
				entry[0].pt[2] * rhs.entry[2].pt[0];
			tmp.entry[1].pt[0] =
				entry[1].pt[0] * rhs.entry[0].pt[0] +
				entry[1].pt[1] * rhs.entry[1].pt[0] +
				entry[1].pt[2] * rhs.entry[2].pt[0];
			tmp.entry[2].pt[0] =
				entry[2].pt[0] * rhs.entry[0].pt[0] +
				entry[2].pt[1] * rhs.entry[1].pt[0] +
				entry[2].pt[2] * rhs.entry[2].pt[0];
			tmp.entry[0].pt[1] =
				entry[0].pt[0] * rhs.entry[0].pt[1] +
				entry[0].pt[1] * rhs.entry[1].pt[1] +
				entry[0].pt[2] * rhs.entry[2].pt[1];
			tmp.entry[1].pt[1] =
				entry[1].pt[0] * rhs.entry[0].pt[1] +
				entry[1].pt[1] * rhs.entry[1].pt[1] +
				entry[1].pt[2] * rhs.entry[2].pt[1];
			tmp.entry[2].pt[1] =
				entry[2].pt[0] * rhs.entry[0].pt[1] +
				entry[2].pt[1] * rhs.entry[1].pt[1] +
				entry[2].pt[2] * rhs.entry[2].pt[1];
			tmp.entry[0].pt[2] =
				entry[0].pt[0] * rhs.entry[0].pt[2] +
				entry[0].pt[1] * rhs.entry[1].pt[2] +
				entry[0].pt[2] * rhs.entry[2].pt[2];
			tmp.entry[1].pt[2] =
				entry[1].pt[0] * rhs.entry[0].pt[2] +
				entry[1].pt[1] * rhs.entry[1].pt[2] +
				entry[1].pt[2] * rhs.entry[2].pt[2];
			tmp.entry[2].pt[2] =
				entry[2].pt[0] * rhs.entry[0].pt[2] +
				entry[2].pt[1] * rhs.entry[1].pt[2] +
				entry[2].pt[2] * rhs.entry[2].pt[2];
			return tmp;
		}

		NiMatrix3 operator*(float scalar) const
		{
			NiMatrix3 result;
			result.entry[0].pt[0] = entry[0].pt[0] * scalar;
			result.entry[0].pt[1] = entry[0].pt[1] * scalar;
			result.entry[0].pt[2] = entry[0].pt[2] * scalar;
			result.entry[1].pt[0] = entry[1].pt[0] * scalar;
			result.entry[1].pt[1] = entry[1].pt[1] * scalar;
			result.entry[1].pt[2] = entry[1].pt[2] * scalar;
			result.entry[2].pt[0] = entry[2].pt[0] * scalar;
			result.entry[2].pt[1] = entry[2].pt[1] * scalar;
			result.entry[2].pt[2] = entry[2].pt[2] * scalar;
			return result;
		}

		NiPoint3 operator*(const NiPoint3& p) const
		{
			return {
				entry[0].pt[0] * p.x + entry[0].pt[1] * p.y + entry[0].pt[2] * p.z,
				entry[1].pt[0] * p.x + entry[1].pt[1] * p.y + entry[1].pt[2] * p.z,
				entry[2].pt[0] * p.x + entry[2].pt[1] * p.y + entry[2].pt[2] * p.z
            };
		}

		RE::NiPoint4& operator[](size_t i)
		{
			assert(i < 3);
			return entry[i];
		}

		const RE::NiPoint4& operator[](size_t i) const
		{
			assert(i < 3);
			return entry[i];
		}
/*
		bool ToEulerAnglesXYZ(float& a_x, float& a_y, float& a_z)
		{
			using func_t = decltype(&NiMatrix3::ToEulerAnglesXYZ);
			REL::Relocation<func_t> func{ ID::NiMatrix3::ToEulerAnglesXYZ };
			return func(this, a_x, a_y, a_z);
		}*/



        //YXZ
//        inline bool ToEulerAnglesYXZ(float& yaw, float& pitch, float& roll) const {
//            pitch = -asin(entry[1][2]);
//            if(pitch >= HALF_PI) {
//                yaw = -atan2(entry[2][0], entry[0][0]);
//                roll = 0.0f;
//            }
//            else {
//                if(pitch > -HALF_PI) {
//                    yaw = -atan2(-entry[1][0], entry[1][1]);
//                    roll = atan2(-entry[0][2], entry[2][2]);
//                    return true;
//                }
//                yaw = atan2(entry[2][0], entry[0][0]);
//                roll = 0.0f;
//            }
//            return false;
//        }

//        inline bool ToEulerAnglesYXZ(float& yaw, float& pitch, float& roll) const {
//            typedef bool (*NiMatrixToEulerYXZfn)(const NiMatrix3*, float&, float&, float&);
//            static auto NiMatrixToEulerYXZ = (NiMatrixToEulerYXZfn)((uintptr_t )MemoryScan::mod + 0x37d9574);
//            return NiMatrixToEulerYXZ(this, yaw, pitch, roll);
//        }
//
//        inline void FromEulerAnglesYXZ(float yaw, float pitch, float roll) {
//            typedef void (*NiMatrixFromEulerYXZfn)(const RE::NiMatrix3*, float, float, float);
//            static auto NiMatrixFromEulerYXZ = (NiMatrixFromEulerYXZfn)((uintptr_t )MemoryScan::mod + 0x37d8a64);
//            NiMatrixFromEulerYXZ(this, yaw, pitch, roll);
//        }

        inline bool ToEulerAnglesXYZ(float& arfXAngle, float& arfYAngle, float& arfZAngle) const {
            arfYAngle = -asin(entry[0][2]);
            if (arfYAngle < HALF_PI) {
                if (arfYAngle > -HALF_PI) {
                    arfXAngle = -atan2(-entry[1][2], entry[2][2]);
                    arfZAngle = -atan2(-entry[0][1], entry[0][0]);
                    return true;
                }
                else {
                    // WARNING.  Not a unique solution.
                    float fRmY = atan2(entry[1][0], entry[1][1]);
                    arfZAngle = 0.0f;  // any angle works
                    arfXAngle = fRmY - arfZAngle;
                    return false;
                }
            }
            else {
                // WARNING.  Not a unique solution.
                float fRpY = atan2(entry[1][0], entry[1][1]);
                arfZAngle = 0.0f;  // any angle works
                arfXAngle = arfZAngle - fRpY;
                return false;
            }
        }

        inline void FromEulerAnglesXYZ(float fXAngle, float fYAngle, float fZAngle) {
            NiMatrix3 kXRot, kYRot, kZRot;
            kXRot.MakeXRotation(fXAngle);
            kYRot.MakeYRotation(fYAngle);
            kZRot.MakeZRotation(fZAngle);
            *this = kXRot * (kYRot * kZRot);
        }

//        inline void FromEulerAnglesYXZ(float yaw, float pitch, float roll) {
//            NiMatrix3 kYRot, kXRot, kZRot;
//            kYRot.MakeYRotation(yaw);
//            kXRot.MakeXRotation(pitch);
//            kZRot.MakeZRotation(roll);
//            *this = kYRot * (kXRot * kZRot);
//        }


        inline void MakeXRotation(float fAngle) {

            float sn = std::sin(fAngle);
            float cs = std::cos(fAngle);

            entry[0][0] = 1.0f;
            entry[0][1] = 0.0f;
            entry[0][2] = 0.0f;
            entry[1][0] = 0.0f;
            entry[1][1] = cs;
            entry[1][2] = sn;
            entry[2][0] = 0.0f;
            entry[2][1] = -sn;
            entry[2][2] = cs;

        }

        inline void MakeYRotation(float fAngle) {

            float sn = std::sin(fAngle);
            float cs = std::cos(fAngle);

            entry[0][0] = cs;
            entry[0][1] = 0.0f;
            entry[0][2] = -sn;
            entry[1][0] = 0.0f;
            entry[1][1] = 1.0f;
            entry[1][2] = 0.0f;
            entry[2][0] = sn;
            entry[2][1] = 0.0f;
            entry[2][2] = cs;

        }

        inline void MakeZRotation(float fAngle) {

            float sn = std::sin(fAngle);
            float cs = std::cos(fAngle);

            entry[0][0] = cs;
            entry[0][1] = sn;
            entry[0][2] = 0.0f;
            entry[1][0] = -sn;
            entry[1][1] = cs;
            entry[1][2] = 0.0f;
            entry[2][0] = 0.0f;
            entry[2][1] = 0.0f;
            entry[2][2] = 1.0f;

        }


		NiMatrix3 Transpose() const
		{
			NiMatrix3 result;
			result.entry[0].pt[0] = entry[0].pt[0];
			result.entry[0].pt[1] = entry[1].pt[0];
			result.entry[0].pt[2] = entry[2].pt[0];
			result.entry[1].pt[0] = entry[0].pt[1];
			result.entry[1].pt[1] = entry[1].pt[1];
			result.entry[1].pt[2] = entry[2].pt[1];
			result.entry[2].pt[0] = entry[0].pt[2];
			result.entry[2].pt[1] = entry[1].pt[2];
			result.entry[2].pt[2] = entry[2].pt[2];
			return result;
		}

		// members
		NiPoint4 entry[3];  // 00
	};
	static_assert(sizeof(NiMatrix3) == 0x30);
}
