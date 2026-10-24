#pragma once

#include "RE/S/ScaleformGFxStateBag.h"
#include "RE/S/ScaleformPtr.h"

namespace RE::Scaleform::GFx
{
	class ASMovieRootBase;

  enum class PlayState
  {
      kPlaying,
      kStopped
  };

  enum class FocusMovedType
  {
      kByMouse = 1,
      kByKeyboard,
      kByAS
  };

	class __declspec(novtable) Resource
	{
	public:
		virtual ~Resource() = default;  // 00

		// add
		virtual void* GetKey();                       // 01
		virtual std::uint32_t GetResourceTypeCode() const;  // 02
		virtual void* GetResourceReport();        // 03

		// members
		uintptr_t refCount;  // 08
		void* lib;              // 10
	};

	class __declspec(novtable) MovieDef :
		public Resource,  // 00
		public StateBag   // 18
	{
	public:

		// add
		virtual std::uint32_t GetVersion() const = 0;       // 04
		virtual std::uint32_t GetLoadingFrame() const = 0;  // 05
		virtual float         GetWidth() const = 0;         // 06
		virtual float         GetHeight() const = 0;        // 07
		virtual std::uint32_t GetFrameCount() const = 0;    // 08
		virtual float         GetFrameRate() const = 0;     // 09
		virtual std::uintptr_t GetFrameRect() const = 0;     // 0A
		virtual std::uint32_t GetSWFFlags() const = 0;      // 0B
		virtual const char*   GetFileURL() const = 0;
	};

	class Movie :
		public RefCountBase<Movie, 327>,  // 00
		public StateBag                   // 10
	{
	public:

		enum class SetVarType : std::uint32_t
		{
			kNormal,
			kSticky,
			kPermanent
		};

		enum class SetArrayType : std::uint32_t
		{
			kInt,
			kDouble,
			kFloat,
			kString,
			kStringW,
			kValue
		};

		virtual ~Movie() = default;  // 01
		virtual MovieDef* GetMovieDef() const = 0;                                                                                                                       // 01

		// members
//		Ptr<ASMovieRootBase> asMovieRoot;
	};

  using MovieDisplayHandle = std::uint64_t;
  using ScaleModeType = std::uint32_t;
  using AlignType = std::uint32_t;
  using RectF = glm::vec4;
  using PointF = glm::vec4;
  using Matrix2F = glm::mat2x2;
  using Matrix3F = glm::mat3x4;
  using Matrix4F = glm::mat4;

  using ThreadId = std::uint32_t;

	class MovieImpl :
		public Movie  // 00
	{
	public:

		virtual ~MovieImpl() = default;  // 00
		virtual MovieDef* GetMovieDef() const = 0;// 01
    virtual std::uint32_t GetCurrentFrame() const = 0;                                                                                                               // 02
    virtual bool HasLooped() const = 0;                                                                                                                              // 03
    virtual void GotoFrame(std::uint32_t a_frameNumber) = 0;                                                                                                         // 04
    virtual bool GotoLabeledFrame(const char* a_label, std::int32_t a_offset = 0) = 0;                                                                               // 05
    virtual void SetPlayState(PlayState a_state) = 0;                                                                                                                // 06
    virtual PlayState GetPlayState() const = 0;                                                                                                                      // 07
    virtual bool IsValid() = 0;                                                                                                                                      // 08
    virtual void SetVisible(bool a_visible) = 0;                                                                                                                     // 09
    virtual bool GetVisible() const = 0;                                                                                                                             // 0A
    virtual std::uint64_t GetASTimerMs() const = 0;                                                                                                                  // 0B
    virtual void SetViewport(void* a_viewDesc) = 0;                                                                                                        // 0C
    virtual void GetViewport(void* a_viewDesc) const = 0;                                                                                                        // 0D
    virtual void SetViewScaleMode(ScaleModeType) = 0;                                                                                                                // 0E
    virtual ScaleModeType GetViewScaleMode() const = 0;                                                                                                              // 0F
    virtual void SetViewAlignment(AlignType) = 0;                                                                                                                    // 10
    virtual AlignType GetViewAlignment() const = 0;                                                                                                                  // 11
    virtual RectF GetVisibleFrameRect() const = 0;                                                                                                                   // 12
    virtual RectF GetSafeRect() const = 0;                                                                                                                           // 13
    virtual void SetSafeRect(const RectF& a_rect) = 0;                                                                                                               // 14
    virtual void SetEdgeAAMode(int a_edgeAA) = 0;                                                                                                     // 15
    virtual int GetEdgeAAMode() const = 0;                                                                                                            // 16
    virtual void Restart(bool a_advance0 = true) = 0;                                                                                                                // 17
    virtual float Advance(float a_deltaT, std::uint32_t a_frameCatchUpCount = 2, bool a_capture = true) = 0;                                                         // 18
    virtual void Capture(bool a_onChangeOnly = true) = 0;                                                                                                            // 19
    virtual const MovieDisplayHandle& GetDisplayHandle() const = 0;                                                                                                  // 1A
    virtual void SetCaptureThread(ThreadId a_captureThreadId) = 0;                                                                                                   // 1B
    virtual void ShutdownRendering(bool a_wait) = 0;                                                                                                                 // 1C
    virtual bool IsShutdownRenderingComplete() const = 0;                                                                                                            // 1D
    virtual void SetPause(bool a_pause) = 0;                                                                                                                         // 1E
    virtual bool IsPaused() const = 0;                                                                                                                               // 1F
    virtual void SetBackgroundColor(int a_bgColor) = 0;                                                                                                    // 20
    virtual void SetBackgroundAlpha(float a_alpha) = 0;                                                                                                              // 21
    virtual float GetBackgroundAlpha() const = 0;                                                                                                                    // 22
    virtual std::uint32_t HandleEvent(const struct Event& a_event) = 0;                                                                                                     // 23
    virtual void GetMouseState(std::uint32_t a_mouseIndex, float* a_x, float* a_y, std::uint32_t* a_buttons) = 0;                                                    // 24
    virtual void NotifyMouseState(float a_x, float a_y, std::uint32_t a_buttons, std::uint32_t a_mouseIndex = 0) = 0;
    /*HitTestType a_testCond = HitTestType::kShapes, std::uint32_t a_controllerIdx = 0*/// 25
    virtual bool HitTest(float a_x, float a_y) = 0;                                // 26
    virtual void* GetUserData() const = 0;                                                                                                                           // 27
    virtual void SetUserData(void*) = 0;                                                                                                                             // 28
    virtual bool IsMovieFocused() const = 0;                                                                                                                         // 29
    virtual bool GetDirtyFlag(bool a_doReset = true) = 0;                                                                                                            // 2A
    virtual void SetMouseCursorCount(std::uint32_t a_num) = 0;                                                                                                       // 2B
    virtual std::uint32_t GetMouseCursorCount() const = 0;                                                                                                           // 2C
    virtual void SetControllerCount(std::uint32_t a_num) = 0;                                                                                                        // 2D
    virtual std::uint32_t GetControllerCount() const = 0;                                                                                                            // 2E
    virtual void GetStats(struct StatBag* a_bag, bool a_reset = true) = 0;                                                                                                  // 2F
    virtual struct MemoryHeap* GetHeap() const = 0;                                                                                                                         // 30
    virtual void ForceCollectGarbage() = 0;                                                                                                                          // 31
    virtual void SuspendGC(bool a_suspend) = 0;                                                                                                                      // 32
    virtual void ScheduleGC() = 0;                                                                                                                                   // 33
    virtual void PrintObjectsReport(/*ReportFlags a_flags = ReportFlags::kNone, Log* a_log = nullptr, const char* a_swfName = nullptr*/) = 0;                            // 34
    virtual PointF TranslateToScreen(const PointF& a_pt, Matrix2F* a_userMatrix = nullptr) = 0;                                                      // 36
    virtual RectF TranslateToScreen1(const RectF& a_pt, Matrix2F* a_userMatrix = nullptr) = 0;                                                                        // 35
    virtual bool TranslateLocalToScreen(const char* a_pathToCharacter, const PointF& a_pt, PointF* a_presPt, Matrix2F* a_userMatrix = nullptr) = 0;  // 37
    virtual bool SetControllerFocusGroup(std::uint32_t a_controllerIdx, std::uint32_t a_focusGroupIndex) = 0;                                                        // 38
    virtual std::uint32_t GetControllerFocusGroup(std::uint32_t a_controllerIdx) const = 0;                                                                          // 39
    virtual void ResetInputFocus(std::uint32_t a_controllerIdx) = 0;                                                                                                 // 3A
    virtual bool IsExitRequested() const = 0;                                                                                                                        // 3B
    virtual void GetViewMatrix3D(Matrix3F*) = 0;                                                                                                                     // 3C
    virtual void SetViewMatrix3D(Matrix3F& a_matrix) = 0;                                                                                                      // 3D
    virtual void GetProjectionMatrix3D(Matrix4F*) = 0;                                                                                                               // 3E
    virtual void SetProjectionMatrix3D(Matrix4F& a_matrix) = 0;                                                                                                // 3F
    virtual void SetStageAutoOrients(bool a_value = true) = 0;                                                                                                       // 40
    virtual void ForceUpdateImages() = 0;                                                                                                                            // 41
    virtual void MakeAreaVisible(const RectF& a_screenRect, const RectF& a_box, int a_flags) = 0;       // 42
    virtual void RestoreViewport() = 0;

  };
}
