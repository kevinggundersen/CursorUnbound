#include "CursorImage.h"

#include <wincodec.h>

namespace CursorUnbound
{
	namespace
	{
		// RAII for the handful of COM pointers below. Pulling in <wrl.h> or <atlbase.h>
		// for five objects is not worth the include cost.
		template <class T>
		class ComPtr
		{
		public:
			ComPtr() = default;
			ComPtr(const ComPtr&) = delete;
			ComPtr& operator=(const ComPtr&) = delete;

			~ComPtr()
			{
				if (_ptr) {
					_ptr->Release();
				}
			}

			T** operator&() noexcept { return &_ptr; }
			T*  operator->() const noexcept { return _ptr; }
			T*  Get() const noexcept { return _ptr; }
			explicit operator bool() const noexcept { return _ptr != nullptr; }

		private:
			T* _ptr = nullptr;
		};

		// The game already lives on an initialised apartment by the time we load, but a
		// plugin cannot assume that. S_FALSE and RPC_E_CHANGED_MODE both mean "COM is
		// usable, someone else set it up", so neither is an error for our purposes.
		class ComScope
		{
		public:
			ComScope()
			{
				const auto hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				_shouldUninitialize = SUCCEEDED(hr) && hr != S_FALSE;
			}

			~ComScope()
			{
				if (_shouldUninitialize) {
					::CoUninitialize();
				}
			}

			ComScope(const ComScope&) = delete;
			ComScope& operator=(const ComScope&) = delete;

		private:
			bool _shouldUninitialize = false;
		};

		bool IsIconFormat(const std::filesystem::path& a_path)
		{
			auto ext = a_path.extension().wstring();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
			return ext == L".cur" || ext == L".ani" || ext == L".ico";
		}

		HCURSOR CreateFromIconFormat(const std::filesystem::path& a_path, float a_scale)
		{
			// LR_DEFAULTSIZE gives the system metric cursor size, which is what we want at
			// scale 1. A non-default scale asks for an explicit pixel size instead.
			int  cx = 0;
			int  cy = 0;
			UINT flags = LR_LOADFROMFILE | LR_DEFAULTSIZE;
			if (a_scale != 1.0f) {
				const auto base = ::GetSystemMetrics(SM_CXCURSOR);
				const auto baseY = ::GetSystemMetrics(SM_CYCURSOR);
				cx = static_cast<int>(std::lround(base * a_scale));
				cy = static_cast<int>(std::lround(baseY * a_scale));
				flags = LR_LOADFROMFILE;
			}

			return static_cast<HCURSOR>(
				::LoadImageW(nullptr, a_path.c_str(), IMAGE_CURSOR, cx, cy, flags));
		}

		HCURSOR CreateFromBitmapFormat(
			const std::filesystem::path& a_path,
			int                          a_hotspotX,
			int                          a_hotspotY,
			float                        a_scale)
		{
			ComScope com;

			ComPtr<IWICImagingFactory> factory;
			auto                       hr = ::CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
			if (FAILED(hr) || !factory) {
				SKSE::log::error("WIC factory creation failed (hr=0x{:08X}).", static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			ComPtr<IWICBitmapDecoder> decoder;
			hr = factory->CreateDecoderFromFilename(
				a_path.c_str(),
				nullptr,
				GENERIC_READ,
				WICDecodeMetadataCacheOnDemand,
				&decoder);
			if (FAILED(hr) || !decoder) {
				SKSE::log::error("Could not decode '{}' (hr=0x{:08X}).", a_path.string(), static_cast<std::uint32_t>(hr));
				return nullptr;
			}

			ComPtr<IWICBitmapFrameDecode> frame;
			hr = decoder->GetFrame(0, &frame);
			if (FAILED(hr) || !frame) {
				return nullptr;
			}

			UINT srcWidth = 0;
			UINT srcHeight = 0;
			hr = frame->GetSize(&srcWidth, &srcHeight);
			if (FAILED(hr) || srcWidth == 0 || srcHeight == 0) {
				return nullptr;
			}

			// Two mistakes are common enough to be worth calling out explicitly, because
			// both produce a "cursor is there but looks wrong" result rather than an error:
			// a source with no alpha channel (opaque rectangle) and a full screenshot
			// pasted in instead of a cropped cursor.
			WICPixelFormatGUID sourceFormat{};
			if (SUCCEEDED(frame->GetPixelFormat(&sourceFormat))) {
				// Ask WIC whether the format carries transparency rather than matching against
				// a hardcoded GUID list, which would go stale with every new SDK format.
				ComPtr<IWICComponentInfo> componentInfo;
				if (SUCCEEDED(factory->CreateComponentInfo(sourceFormat, &componentInfo)) && componentInfo) {
					ComPtr<IWICPixelFormatInfo2> formatInfo;
					if (SUCCEEDED(componentInfo->QueryInterface(IID_PPV_ARGS(&formatInfo))) && formatInfo) {
						BOOL supportsTransparency = FALSE;
						if (SUCCEEDED(formatInfo->SupportsTransparency(&supportsTransparency)) &&
							!supportsTransparency) {
							SKSE::log::warn(
								"'{}' has no alpha channel. It will draw as an opaque rectangle. "
								"Export it as 32-bit RGBA (PNG with transparency).",
								a_path.string());
						}
					}
				}
			}

			if (srcWidth > 256 || srcHeight > 256) {
				SKSE::log::warn(
					"'{}' is {}x{}, which is far larger than a cursor (32x32 is typical). "
					"Crop it to just the pointer, or set Scale to shrink it.",
					a_path.string(), srcWidth, srcHeight);
			}

			const auto width = std::max<UINT>(1, static_cast<UINT>(std::lround(srcWidth * a_scale)));
			const auto height = std::max<UINT>(1, static_cast<UINT>(std::lround(srcHeight * a_scale)));

			// Keep the scaler alive until CopyPixels has run - the converter only holds a
			// borrowed reference to whatever source it was initialised with.
			ComPtr<IWICBitmapScaler> scaler;
			IWICBitmapSource*        source = frame.Get();
			if (width != srcWidth || height != srcHeight) {
				hr = factory->CreateBitmapScaler(&scaler);
				if (SUCCEEDED(hr) && scaler) {
					hr = scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant);
					if (SUCCEEDED(hr)) {
						source = scaler.Get();
					}
				}
			}

			ComPtr<IWICFormatConverter> converter;
			hr = factory->CreateFormatConverter(&converter);
			if (FAILED(hr) || !converter) {
				return nullptr;
			}

			// PBGRA is what CreateIconIndirect wants for an alpha-blended cursor.
			hr = converter->Initialize(
				source,
				GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone,
				nullptr,
				0.0,
				WICBitmapPaletteTypeMedianCut);
			if (FAILED(hr)) {
				return nullptr;
			}

			BITMAPINFO bmi{};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = static_cast<LONG>(width);
			bmi.bmiHeader.biHeight = -static_cast<LONG>(height);  // negative => top-down
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			void*   bits = nullptr;
			HBITMAP colorBitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (!colorBitmap || !bits) {
				if (colorBitmap) {
					::DeleteObject(colorBitmap);
				}
				return nullptr;
			}

			const UINT stride = width * 4;
			hr = converter->CopyPixels(nullptr, stride, stride * height, static_cast<BYTE*>(bits));
			if (FAILED(hr)) {
				::DeleteObject(colorBitmap);
				return nullptr;
			}

			// An all-zero AND mask means "take everything from the colour bitmap", which
			// lets the 32-bit alpha channel drive blending. Monochrome rows are WORD aligned.
			const std::size_t  maskStride = ((static_cast<std::size_t>(width) + 15) / 16) * 2;
			std::vector<BYTE>  maskBits(maskStride * height, 0);
			HBITMAP            maskBitmap = ::CreateBitmap(
                static_cast<int>(width),
                static_cast<int>(height),
                1,
                1,
                maskBits.data());
			if (!maskBitmap) {
				::DeleteObject(colorBitmap);
				return nullptr;
			}

			ICONINFO info{};
			info.fIcon = FALSE;  // FALSE => cursor, which is what makes the hotspot matter
			info.xHotspot = static_cast<DWORD>(std::clamp(
				static_cast<int>(std::lround(a_hotspotX * a_scale)), 0, static_cast<int>(width) - 1));
			info.yHotspot = static_cast<DWORD>(std::clamp(
				static_cast<int>(std::lround(a_hotspotY * a_scale)), 0, static_cast<int>(height) - 1));
			info.hbmMask = maskBitmap;
			info.hbmColor = colorBitmap;

			HCURSOR cursor = ::CreateIconIndirect(&info);

			::DeleteObject(maskBitmap);
			::DeleteObject(colorBitmap);

			if (!cursor) {
				SKSE::log::error("CreateIconIndirect failed for '{}'.", a_path.string());
				return nullptr;
			}

			SKSE::log::info(
				"Built cursor from '{}' at {}x{}, hotspot ({}, {}).",
				a_path.string(),
				width,
				height,
				info.xHotspot,
				info.yHotspot);

			return cursor;
		}
	}

	HCURSOR CreateCursorFromFile(
		const std::filesystem::path& a_path,
		int                          a_hotspotX,
		int                          a_hotspotY,
		float                        a_scale)
	{
		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			SKSE::log::warn("Cursor file '{}' does not exist.", a_path.string());
			return nullptr;
		}

		if (IsIconFormat(a_path)) {
			// .cur/.ani carry their own hotspot, so the configured one is ignored here.
			HCURSOR cursor = CreateFromIconFormat(a_path, a_scale);
			if (cursor) {
				SKSE::log::info("Loaded cursor '{}' (native cursor format).", a_path.string());
			} else {
				SKSE::log::error("LoadImage failed for '{}'.", a_path.string());
			}
			return cursor;
		}

		return CreateFromBitmapFormat(a_path, a_hotspotX, a_hotspotY, a_scale);
	}

	std::optional<std::filesystem::path> FindCursorFile(const std::filesystem::path& a_directory)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(a_directory, ec)) {
			return std::nullopt;
		}

		constexpr std::array preferred{ L"cursor.ani", L"cursor.cur", L"cursor.png" };
		for (const auto* name : preferred) {
			auto candidate = a_directory / name;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
		}

		constexpr std::array supported{
			L".ani", L".cur", L".ico", L".png", L".bmp", L".tif", L".tiff", L".jpg", L".jpeg", L".gif"
		};

		for (const auto& entry : std::filesystem::directory_iterator(a_directory, ec)) {
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			auto ext = entry.path().extension().wstring();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
			if (std::find(supported.begin(), supported.end(), ext) != supported.end()) {
				return entry.path();
			}
		}

		return std::nullopt;
	}
}
