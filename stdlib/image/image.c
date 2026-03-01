#include "../../src/prefix_extension.h"
#include "../../src/interpreter.h"
#include "../../src/value.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "ole32.lib")
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

typedef struct {
	Tensor* t;
	size_t w;
	size_t h;
} ImageView;

static void set_runtime_error(Interpreter* interp, const char* msg, int line, int col) {
	if (!interp) return;
	if (interp->error) free(interp->error);
#ifdef _MSC_VER
	interp->error = msg ? _strdup(msg) : NULL;
#else
	interp->error = msg ? strdup(msg) : NULL;
#endif
	interp->error_line = line;
	interp->error_col = col;
}

static Value fail(Interpreter* interp, const char* msg, int line, int col) {
	set_runtime_error(interp, msg, line, col);
	return value_null();
}

static int expect_argc_range(Interpreter* interp, int argc, int minc, int maxc, const char* opname, int line, int col) {
	if (argc < minc || argc > maxc) {
		char buf[192];
		snprintf(buf, sizeof(buf), "%s expects %d..%d arguments", opname, minc, maxc);
		set_runtime_error(interp, buf, line, col);
		return 0;
	}
	return 1;
}

static int64_t expect_int(Interpreter* interp, Value v, const char* opname, int line, int col) {
	if (v.type != VAL_INT) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects INT argument", opname);
		set_runtime_error(interp, buf, line, col);
		return 0;
	}
	return v.as.i;
}

static double expect_num(Interpreter* interp, Value v, const char* opname, int line, int col) {
	if (v.type == VAL_FLT) return v.as.f;
	if (v.type == VAL_INT) return (double)v.as.i;
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects FLT/INT numeric argument", opname);
		set_runtime_error(interp, buf, line, col);
	}
	return 0.0;
}

static const char* expect_str(Interpreter* interp, Value v, const char* opname, int line, int col) {
	if (v.type != VAL_STR) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects STR argument", opname);
		set_runtime_error(interp, buf, line, col);
		return NULL;
	}
	return v.as.s ? v.as.s : "";
}

static int image_from_value(Interpreter* interp, Value v, const char* opname, int line, int col, ImageView* out) {
	if (!out) return 0;
	if (v.type != VAL_TNS || !v.as.tns) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects TNS image", opname);
		set_runtime_error(interp, buf, line, col);
		return 0;
	}
	Tensor* t = v.as.tns;
	if (t->ndim != 3 || t->shape[2] != 4) {
		char buf[192];
		snprintf(buf, sizeof(buf), "%s expects image shape [width,height,4]", opname);
		set_runtime_error(interp, buf, line, col);
		return 0;
	}
	if (t->shape[0] == 0 || t->shape[1] == 0) {
		set_runtime_error(interp, "image dimensions must be non-zero", line, col);
		return 0;
	}
	out->t = t;
	out->w = t->shape[0];
	out->h = t->shape[1];
	return 1;
}

static size_t pixel_offset(const Tensor* t, size_t x, size_t y) {
	return (x * t->strides[0]) + (y * t->strides[1]);
}

static int clamp_u8_i64(int64_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (int)v;
}

static int clamp_u8_i32(int v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return v;
}

static Value make_image(size_t w, size_t h) {
	size_t shape[3];
	shape[0] = w;
	shape[1] = h;
	shape[2] = 4;
	return value_tns_new(TYPE_INT, 3, shape);
}

static Value copy_image_checked(Interpreter* interp, Value src, const char* opname, int line, int col) {
	ImageView iv;
	if (!image_from_value(interp, src, opname, line, col, &iv)) return value_null();
	Value out = make_image(iv.w, iv.h);
	Tensor* st = iv.t;
	Tensor* dt = out.as.tns;
	for (size_t x = 0; x < iv.w; x++) {
		for (size_t y = 0; y < iv.h; y++) {
			size_t so = pixel_offset(st, x, y);
			size_t doff = pixel_offset(dt, x, y);
			for (size_t c = 0; c < 4; c++) {
				Value sv = st->data[so + c];
				if (sv.type != VAL_INT) {
					value_free(out);
					set_runtime_error(interp, "image tensor channels must be INT", line, col);
					return value_null();
				}
				dt->data[doff + c] = value_int((int64_t)clamp_u8_i64(sv.as.i));
			}
		}
	}
	return out;
}

static int parse_color_rgba(Interpreter* interp, Value v, int out_rgba[4], const char* opname, int line, int col) {
	(void)opname;
	if (v.type != VAL_TNS || !v.as.tns) {
		set_runtime_error(interp, "color must be TNS[4]", line, col);
		return 0;
	}
	Tensor* t = v.as.tns;
	if (!(t->ndim == 1 && t->shape[0] == 4)) {
		set_runtime_error(interp, "color must be shape [4]", line, col);
		return 0;
	}
	for (size_t i = 0; i < 4; i++) {
		Value e = t->data[i];
		if (e.type != VAL_INT) {
			set_runtime_error(interp, "color channels must be INT", line, col);
			return 0;
		}
		out_rgba[i] = clamp_u8_i64(e.as.i);
	}
	return 1;
}

static int parse_points_xy(Interpreter* interp, Value v, int** out_pts, size_t* out_count, int line, int col) {
	if (!out_pts || !out_count) return 0;
	*out_pts = NULL;
	*out_count = 0;
	if (v.type != VAL_TNS || !v.as.tns) {
		set_runtime_error(interp, "points must be TNS", line, col);
		return 0;
	}
	Tensor* t = v.as.tns;
	if (t->ndim != 2 || t->shape[1] != 2 || t->shape[0] < 2) {
		set_runtime_error(interp, "points must be shape [N,2], N>=2", line, col);
		return 0;
	}
	size_t n = t->shape[0];
	int* pts = (int*)malloc(sizeof(int) * (n * 2));
	if (!pts) {
		set_runtime_error(interp, "out of memory", line, col);
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		size_t off = i * t->strides[0];
		Value vx = t->data[off + 0];
		Value vy = t->data[off + 1];
		if (vx.type != VAL_INT || vy.type != VAL_INT) {
			free(pts);
			set_runtime_error(interp, "point coordinates must be INT", line, col);
			return 0;
		}
		/* Convert from user (1-based) coordinates to internal (0-based) */
		pts[i * 2 + 0] = (int)vx.as.i - 1;
		pts[i * 2 + 1] = (int)vy.as.i - 1;
	}
	*out_pts = pts;
	*out_count = n;
	return 1;
}

static void put_pixel_rgba(Tensor* t, int x, int y, const int rgba[4], int mix_alpha) {
	if (!t) return;
	if (x < 0 || y < 0) return;
	if ((size_t)x >= t->shape[0] || (size_t)y >= t->shape[1]) return;
	size_t off = pixel_offset(t, (size_t)x, (size_t)y);
	int dr = (int)t->data[off + 0].as.i;
	int dg = (int)t->data[off + 1].as.i;
	int db = (int)t->data[off + 2].as.i;
	int da = (int)t->data[off + 3].as.i;
	int sr = rgba[0], sg = rgba[1], sb = rgba[2], sa = rgba[3];
	if (!mix_alpha) {
		t->data[off + 0] = value_int(sr);
		t->data[off + 1] = value_int(sg);
		t->data[off + 2] = value_int(sb);
		t->data[off + 3] = value_int(sa);
		return;
	}
	int inv = 255 - sa;
	int orr = clamp_u8_i32((sa * sr + inv * dr) / 255);
	int org = clamp_u8_i32((sa * sg + inv * dg) / 255);
	int orb = clamp_u8_i32((sa * sb + inv * db) / 255);
	int ora = clamp_u8_i32(sa + (inv * da) / 255);
	t->data[off + 0] = value_int(orr);
	t->data[off + 1] = value_int(org);
	t->data[off + 2] = value_int(orb);
	t->data[off + 3] = value_int(ora);
}

static void draw_line(Tensor* t, int x0, int y0, int x1, int y1, const int rgba[4], int thickness) {
	int dx = abs(x1 - x0);
	int sx = (x0 < x1) ? 1 : -1;
	int dy = -abs(y1 - y0);
	int sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	int half = thickness > 1 ? (thickness / 2) : 0;

	for (;;) {
		for (int ox = -half; ox <= half; ox++) {
			for (int oy = -half; oy <= half; oy++) {
				put_pixel_rgba(t, x0 + ox, y0 + oy, rgba, 1);
			}
		}
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) {
			if (x0 == x1) break;
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			if (y0 == y1) break;
			err += dx;
			y0 += sy;
		}
	}
}

static void fill_polygon(Tensor* t, const int* pts, size_t npts, const int rgba[4]) {
	if (!t || !pts || npts < 3) return;
	int miny = pts[1], maxy = pts[1];
	for (size_t i = 1; i < npts; i++) {
		int y = pts[i * 2 + 1];
		if (y < miny) miny = y;
		if (y > maxy) maxy = y;
	}
	if (miny < 0) miny = 0;
	if (maxy >= (int)t->shape[1]) maxy = (int)t->shape[1] - 1;

	int* nodes = (int*)malloc(sizeof(int) * npts);
	if (!nodes) return;

	for (int y = miny; y <= maxy; y++) {
		int count = 0;
		for (size_t i = 0, j = npts - 1; i < npts; j = i++) {
			int xi = pts[i * 2 + 0], yi = pts[i * 2 + 1];
			int xj = pts[j * 2 + 0], yj = pts[j * 2 + 1];
			int cond = ((yi < y && yj >= y) || (yj < y && yi >= y));
			if (cond && (yj != yi)) {
				int x = xi + (int)((double)(y - yi) * (double)(xj - xi) / (double)(yj - yi));
				nodes[count++] = x;
			}
		}
		for (int i = 0; i < count - 1; i++) {
			for (int j = i + 1; j < count; j++) {
				if (nodes[j] < nodes[i]) {
					int tmp = nodes[i];
					nodes[i] = nodes[j];
					nodes[j] = tmp;
				}
			}
		}
		for (int i = 0; i + 1 < count; i += 2) {
			int x0 = nodes[i];
			int x1 = nodes[i + 1];
			if (x0 < 0) x0 = 0;
			if (x1 >= (int)t->shape[0]) x1 = (int)t->shape[0] - 1;
			for (int x = x0; x <= x1; x++) put_pixel_rgba(t, x, y, rgba, 1);
		}
	}
	free(nodes);
}

static void draw_polygon(Tensor* t, const int* pts, size_t npts, const int rgba[4], int fill, int thickness) {
	if (!t || !pts || npts < 2) return;
	if (fill) fill_polygon(t, pts, npts, rgba);
	for (size_t i = 0; i < npts - 1; i++) {
		int x0 = pts[i * 2 + 0], y0 = pts[i * 2 + 1];
		int x1 = pts[(i + 1) * 2 + 0], y1 = pts[(i + 1) * 2 + 1];
		draw_line(t, x0, y0, x1, y1, rgba, thickness);
	}
	draw_line(t, pts[(npts - 1) * 2 + 0], pts[(npts - 1) * 2 + 1], pts[0], pts[1], rgba, thickness);
}

static void draw_ellipse(Tensor* t, int cx, int cy, int rx, int ry, const int rgba[4], int fill, int thickness) {
	if (!t || rx <= 0 || ry <= 0) return;
	if (fill) {
		for (int y = -ry; y <= ry; y++) {
			double yf = (double)y / (double)ry;
			double xr = (1.0 - yf * yf);
			if (xr < 0.0) continue;
			int dx = (int)floor(sqrt(xr) * (double)rx + 0.5);
			for (int x = -dx; x <= dx; x++) put_pixel_rgba(t, cx + x, cy + y, rgba, 1);
		}
	}
	int steps = (int)(2.0 * 3.14159265358979323846 * (double)(rx > ry ? rx : ry));
	if (steps < 32) steps = 32;
	int half = thickness > 1 ? thickness / 2 : 0;
	for (int i = 0; i <= steps; i++) {
		double a = (double)i * 2.0 * 3.14159265358979323846 / (double)steps;
		int x = cx + (int)floor(cos(a) * (double)rx + 0.5);
		int y = cy + (int)floor(sin(a) * (double)ry + 0.5);
		for (int ox = -half; ox <= half; ox++) {
			for (int oy = -half; oy <= half; oy++) {
				put_pixel_rgba(t, x + ox, y + oy, rgba, 1);
			}
		}
	}
}

#ifdef _WIN32
typedef struct {
	UINT32 GdiplusVersion;
	void* DebugEventCallback;
	BOOL SuppressBackgroundThread;
	BOOL SuppressExternalCodecs;
} GdiplusStartupInput_C;

typedef struct {
	UINT Width;
	UINT Height;
	INT Stride;
	INT PixelFormat;
	void* Scan0;
	UINT_PTR Reserved;
} BitmapData_C;

typedef struct {
	INT X;
	INT Y;
	INT Width;
	INT Height;
} GpRect_C;

typedef struct _EncoderParameter {
	GUID Guid;
	ULONG NumberOfValues;
	ULONG Type;
	void* Value;
} EncoderParameter_C;

typedef struct _EncoderParameters {
	UINT Count;
	EncoderParameter_C Parameter[1];
} EncoderParameters_C;

typedef struct _ImageCodecInfo {
	CLSID Clsid;
	GUID FormatID;
	const WCHAR* CodecName;
	const WCHAR* DllName;
	const WCHAR* FormatDescription;
	const WCHAR* FilenameExtension;
	const WCHAR* MimeType;
	DWORD Flags;
	DWORD Version;
	DWORD SigCount;
	DWORD SigSize;
	const BYTE* SigPattern;
	const BYTE* SigMask;
} ImageCodecInfo_C;

typedef void GpImage_C;
typedef void GpBitmap_C;
typedef int GpStatus;

typedef GpStatus(WINAPI* fnGdiplusStartup)(ULONG_PTR*, const GdiplusStartupInput_C*, void*);
typedef void(WINAPI* fnGdiplusShutdown)(ULONG_PTR);
typedef GpStatus(WINAPI* fnGdipLoadImageFromFile)(const WCHAR*, GpImage_C**);
typedef GpStatus(WINAPI* fnGdipGetImageWidth)(GpImage_C*, UINT*);
typedef GpStatus(WINAPI* fnGdipGetImageHeight)(GpImage_C*, UINT*);
typedef GpStatus(WINAPI* fnGdipBitmapLockBits)(GpBitmap_C*, const GpRect_C*, UINT, INT, BitmapData_C*);
typedef GpStatus(WINAPI* fnGdipBitmapUnlockBits)(GpBitmap_C*, BitmapData_C*);
typedef GpStatus(WINAPI* fnGdipDisposeImage)(GpImage_C*);
typedef GpStatus(WINAPI* fnGdipCreateBitmapFromScan0)(INT, INT, INT, INT, BYTE*, GpBitmap_C**);
typedef GpStatus(WINAPI* fnGdipSaveImageToFile)(GpImage_C*, const WCHAR*, const CLSID*, const EncoderParameters_C*);
typedef GpStatus(WINAPI* fnGdipGetImageEncodersSize)(UINT*, UINT*);
typedef GpStatus(WINAPI* fnGdipGetImageEncoders)(UINT, UINT, ImageCodecInfo_C*);

static HMODULE g_gdiplus = NULL;
static ULONG_PTR g_gdiplus_token = 0;
static int g_gdiplus_ready = 0;

static fnGdiplusStartup pGdiplusStartup = NULL;
static fnGdiplusShutdown pGdiplusShutdown = NULL;
static fnGdipLoadImageFromFile pGdipLoadImageFromFile = NULL;
static fnGdipGetImageWidth pGdipGetImageWidth = NULL;
static fnGdipGetImageHeight pGdipGetImageHeight = NULL;
static fnGdipBitmapLockBits pGdipBitmapLockBits = NULL;
static fnGdipBitmapUnlockBits pGdipBitmapUnlockBits = NULL;
static fnGdipDisposeImage pGdipDisposeImage = NULL;
static fnGdipCreateBitmapFromScan0 pGdipCreateBitmapFromScan0 = NULL;
static fnGdipSaveImageToFile pGdipSaveImageToFile = NULL;
static fnGdipGetImageEncodersSize pGdipGetImageEncodersSize = NULL;
static fnGdipGetImageEncoders pGdipGetImageEncoders = NULL;

#define PixelFormat32bppARGB_C 0x26200A
#define ImageLockModeRead_C 1

static int utf8_to_wide(const char* s, WCHAR** out_w) {
	if (!s || !out_w) return 0;
	*out_w = NULL;
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) return 0;
	WCHAR* w = (WCHAR*)malloc(sizeof(WCHAR) * (size_t)n);
	if (!w) return 0;
	if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		return 0;
	}
	*out_w = w;
	return 1;
}

static int streqi_w(const WCHAR* a, const WCHAR* b) {
	if (!a || !b) return 0;
	while (*a && *b) {
		WCHAR ca = *a;
		WCHAR cb = *b;
		if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca + (L'a' - L'A'));
		if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb + (L'a' - L'A'));
		if (ca != cb) return 0;
		a++;
		b++;
	}
	return (*a == 0 && *b == 0) ? 1 : 0;
}

static int ensure_gdiplus(Interpreter* interp, int line, int col) {
	if (g_gdiplus_ready) return 1;
	g_gdiplus = LoadLibraryA("gdiplus.dll");
	if (!g_gdiplus) {
		set_runtime_error(interp, "image: failed to load gdiplus.dll", line, col);
		return 0;
	}
	pGdiplusStartup = (fnGdiplusStartup)GetProcAddress(g_gdiplus, "GdiplusStartup");
	pGdiplusShutdown = (fnGdiplusShutdown)GetProcAddress(g_gdiplus, "GdiplusShutdown");
	pGdipLoadImageFromFile = (fnGdipLoadImageFromFile)GetProcAddress(g_gdiplus, "GdipLoadImageFromFile");
	pGdipGetImageWidth = (fnGdipGetImageWidth)GetProcAddress(g_gdiplus, "GdipGetImageWidth");
	pGdipGetImageHeight = (fnGdipGetImageHeight)GetProcAddress(g_gdiplus, "GdipGetImageHeight");
	pGdipBitmapLockBits = (fnGdipBitmapLockBits)GetProcAddress(g_gdiplus, "GdipBitmapLockBits");
	pGdipBitmapUnlockBits = (fnGdipBitmapUnlockBits)GetProcAddress(g_gdiplus, "GdipBitmapUnlockBits");
	pGdipDisposeImage = (fnGdipDisposeImage)GetProcAddress(g_gdiplus, "GdipDisposeImage");
	pGdipCreateBitmapFromScan0 = (fnGdipCreateBitmapFromScan0)GetProcAddress(g_gdiplus, "GdipCreateBitmapFromScan0");
	pGdipSaveImageToFile = (fnGdipSaveImageToFile)GetProcAddress(g_gdiplus, "GdipSaveImageToFile");
	pGdipGetImageEncodersSize = (fnGdipGetImageEncodersSize)GetProcAddress(g_gdiplus, "GdipGetImageEncodersSize");
	pGdipGetImageEncoders = (fnGdipGetImageEncoders)GetProcAddress(g_gdiplus, "GdipGetImageEncoders");

	if (!pGdiplusStartup || !pGdiplusShutdown || !pGdipLoadImageFromFile || !pGdipGetImageWidth ||
		!pGdipGetImageHeight || !pGdipBitmapLockBits || !pGdipBitmapUnlockBits || !pGdipDisposeImage ||
		!pGdipCreateBitmapFromScan0 || !pGdipSaveImageToFile || !pGdipGetImageEncodersSize || !pGdipGetImageEncoders) {
		set_runtime_error(interp, "image: gdiplus symbols unavailable", line, col);
		return 0;
	}

	GdiplusStartupInput_C in;
	in.GdiplusVersion = 1;
	in.DebugEventCallback = NULL;
	in.SuppressBackgroundThread = FALSE;
	in.SuppressExternalCodecs = FALSE;
	if (pGdiplusStartup(&g_gdiplus_token, &in, NULL) != 0) {
		set_runtime_error(interp, "image: GdiplusStartup failed", line, col);
		return 0;
	}
	g_gdiplus_ready = 1;
	return 1;
}

static int encoder_clsid_for_mime(const WCHAR* mime, CLSID* out_clsid) {
	UINT n = 0;
	UINT sz = 0;
	if (!out_clsid) return 0;
	if (pGdipGetImageEncodersSize(&n, &sz) != 0 || n == 0 || sz == 0) return 0;
	ImageCodecInfo_C* infos = (ImageCodecInfo_C*)malloc((size_t)sz);
	if (!infos) return 0;
	int ok = 0;
	if (pGdipGetImageEncoders(n, sz, infos) == 0) {
		for (UINT i = 0; i < n; i++) {
			if (infos[i].MimeType && streqi_w(infos[i].MimeType, mime)) {
				*out_clsid = infos[i].Clsid;
				ok = 1;
				break;
			}
		}
	}
	free(infos);
	return ok;
}

static Value load_with_gdiplus(Interpreter* interp, const char* path, int line, int col) {
	if (!ensure_gdiplus(interp, line, col)) return value_null();
	WCHAR* wpath = NULL;
	if (!utf8_to_wide(path, &wpath)) return fail(interp, "image: invalid UTF-8 path", line, col);

	GpImage_C* img = NULL;
	if (pGdipLoadImageFromFile(wpath, &img) != 0 || !img) {
		free(wpath);
		return fail(interp, "image: failed to load image file", line, col);
	}
	free(wpath);

	UINT w = 0, h = 0;
	if (pGdipGetImageWidth(img, &w) != 0 || pGdipGetImageHeight(img, &h) != 0 || w == 0 || h == 0) {
		pGdipDisposeImage(img);
		return fail(interp, "image: failed to read image dimensions", line, col);
	}

	GpRect_C rect;
	rect.X = 0;
	rect.Y = 0;
	rect.Width = (INT)w;
	rect.Height = (INT)h;
	BitmapData_C bd;
	memset(&bd, 0, sizeof(bd));
	if (pGdipBitmapLockBits((GpBitmap_C*)img, &rect, ImageLockModeRead_C, PixelFormat32bppARGB_C, &bd) != 0) {
		pGdipDisposeImage(img);
		return fail(interp, "image: failed to lock bitmap", line, col);
	}

	Value out = make_image((size_t)w, (size_t)h);
	Tensor* t = out.as.tns;
	int stride = bd.Stride;
	int abs_stride = (stride >= 0) ? stride : -stride;
	const uint8_t* base = (const uint8_t*)bd.Scan0;
	if (!base) {
		pGdipBitmapUnlockBits((GpBitmap_C*)img, &bd);
		pGdipDisposeImage(img);
		value_free(out);
		return fail(interp, "image: bitmap data is null", line, col);
	}

	for (UINT y = 0; y < h; y++) {
		UINT sy = (stride >= 0) ? y : (h - 1U - y);
		const uint8_t* row = base + (size_t)sy * (size_t)abs_stride;
		for (UINT x = 0; x < w; x++) {
			const uint8_t* px = row + (size_t)x * 4U;
			uint8_t b = px[0], g = px[1], r = px[2], a = px[3];
			size_t off = pixel_offset(t, (size_t)x, (size_t)y);
			t->data[off + 0] = value_int((int64_t)r);
			t->data[off + 1] = value_int((int64_t)g);
			t->data[off + 2] = value_int((int64_t)b);
			t->data[off + 3] = value_int((int64_t)a);
		}
	}

	pGdipBitmapUnlockBits((GpBitmap_C*)img, &bd);
	pGdipDisposeImage(img);
	return out;
}

static Value save_with_gdiplus(Interpreter* interp, Value imgv, const char* path, const WCHAR* mime, int quality, int line, int col) {
	ImageView iv;
	if (!image_from_value(interp, imgv, "SAVE_*", line, col, &iv)) return value_int(0);
	if (!ensure_gdiplus(interp, line, col)) return value_int(0);

	int w = (int)iv.w;
	int h = (int)iv.h;
	int stride = w * 4;
	uint8_t* bgra = (uint8_t*)malloc((size_t)stride * (size_t)h);
	if (!bgra) { set_runtime_error(interp, "image: out of memory", line, col); return value_int(0); }

	for (int y = 0; y < h; y++) {
		uint8_t* row = bgra + (size_t)y * (size_t)stride;
		for (int x = 0; x < w; x++) {
			size_t off = pixel_offset(iv.t, (size_t)x, (size_t)y);
			int r = clamp_u8_i64(iv.t->data[off + 0].as.i);
			int g = clamp_u8_i64(iv.t->data[off + 1].as.i);
			int b = clamp_u8_i64(iv.t->data[off + 2].as.i);
			int a = clamp_u8_i64(iv.t->data[off + 3].as.i);
			row[(size_t)x * 4U + 0U] = (uint8_t)b;
			row[(size_t)x * 4U + 1U] = (uint8_t)g;
			row[(size_t)x * 4U + 2U] = (uint8_t)r;
			row[(size_t)x * 4U + 3U] = (uint8_t)a;
		}
	}

	GpBitmap_C* bmp = NULL;
	if (pGdipCreateBitmapFromScan0(w, h, stride, PixelFormat32bppARGB_C, bgra, &bmp) != 0 || !bmp) {
		free(bgra);
		set_runtime_error(interp, "image: failed to create bitmap", line, col);
		return value_int(0);
	}

	CLSID clsid;
	if (!encoder_clsid_for_mime(mime, &clsid)) {
		pGdipDisposeImage((GpImage_C*)bmp);
		free(bgra);
		set_runtime_error(interp, "image: encoder unavailable", line, col);
		return value_int(0);
	}

	WCHAR* wpath = NULL;
	if (!utf8_to_wide(path, &wpath)) {
		pGdipDisposeImage((GpImage_C*)bmp);
		free(bgra);
		free(wpath);
		set_runtime_error(interp, "image: invalid UTF-8 path", line, col);
		return value_int(0);
	}

	static const GUID EncoderQuality = {0x1d5be4b5, 0xfa4a, 0x452d, {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}};
	ULONG q = (ULONG)(quality < 0 ? 0 : (quality > 100 ? 100 : quality));
	EncoderParameters_C ep;
	ep.Count = 1;
	ep.Parameter[0].Guid = EncoderQuality;
	ep.Parameter[0].NumberOfValues = 1;
	ep.Parameter[0].Type = 4;
	ep.Parameter[0].Value = &q;

	GpStatus st = pGdipSaveImageToFile((GpImage_C*)bmp, wpath, &clsid, &ep);
	free(wpath);
	pGdipDisposeImage((GpImage_C*)bmp);
	free(bgra);

	if (st != 0) { set_runtime_error(interp, "image: failed to save image", line, col); return value_int(0); }
	return value_int(1);
}
#endif

static Value op_load_png(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 1, 1, "LOAD_PNG", line, col)) return value_null();
	const char* path = expect_str(interp, args[0], "LOAD_PNG", line, col);
	if (interp->error) return value_null();
#ifdef _WIN32
	return load_with_gdiplus(interp, path, line, col);
#else
	(void)path;
	return fail(interp, "LOAD_PNG not supported on this platform", line, col);
#endif
}

static Value op_load_jpeg(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 1, 1, "LOAD_JPEG", line, col)) return value_null();
	const char* path = expect_str(interp, args[0], "LOAD_JPEG", line, col);
	if (interp->error) return value_null();
#ifdef _WIN32
	return load_with_gdiplus(interp, path, line, col);
#else
	(void)path;
	return fail(interp, "LOAD_JPEG not supported on this platform", line, col);
#endif
}

static Value op_load_bmp(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 1, 1, "LOAD_BMP", line, col)) return value_null();
	const char* path = expect_str(interp, args[0], "LOAD_BMP", line, col);
	if (interp->error) return value_null();
#ifdef _WIN32
	return load_with_gdiplus(interp, path, line, col);
#else
	(void)path;
	return fail(interp, "LOAD_BMP not supported on this platform", line, col);
#endif
}

static Value op_save_png(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "SAVE_PNG", line, col)) return value_null();
	const char* path = expect_str(interp, args[1], "SAVE_PNG", line, col);
	if (interp->error) return value_int(0);
	int quality = 100;
	if (argc >= 3) quality = (int)expect_int(interp, args[2], "SAVE_PNG", line, col);
	if (interp->error) return value_null();
#ifdef _WIN32
	return save_with_gdiplus(interp, args[0], path, L"image/png", quality, line, col);
#else
	(void)quality;
	set_runtime_error(interp, "SAVE_PNG not supported on this platform", line, col);
	return value_int(0);
#endif
}

static Value op_save_jpeg(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "SAVE_JPEG", line, col)) return value_null();
	const char* path = expect_str(interp, args[1], "SAVE_JPEG", line, col);
	if (interp->error) return value_int(0);
	int quality = 85;
	if (argc >= 3) quality = (int)expect_int(interp, args[2], "SAVE_JPEG", line, col);
	if (interp->error) return value_null();
#ifdef _WIN32
	return save_with_gdiplus(interp, args[0], path, L"image/jpeg", quality, line, col);
#else
	(void)quality;
	set_runtime_error(interp, "SAVE_JPEG not supported on this platform", line, col);
	return value_int(0);
#endif
}

static Value op_save_bmp(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 2, "SAVE_BMP", line, col)) return value_null();
	const char* path = expect_str(interp, args[1], "SAVE_BMP", line, col);
	if (interp->error) return value_int(0);
#ifdef _WIN32
	return save_with_gdiplus(interp, args[0], path, L"image/bmp", 100, line, col);
#else
	set_runtime_error(interp, "SAVE_BMP not supported on this platform", line, col);
	return value_int(0);
#endif
}

static Value op_polygon(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 3, 5, "POLYGON", line, col)) return value_null();
	Value out = copy_image_checked(interp, args[0], "POLYGON", line, col);
	if (interp->error) return value_null();

	int* pts = NULL;
	size_t npts = 0;
	if (!parse_points_xy(interp, args[1], &pts, &npts, line, col)) {
		value_free(out);
		return value_null();
	}
	int color[4];
	if (!parse_color_rgba(interp, args[2], color, "POLYGON", line, col)) {
		free(pts);
		value_free(out);
		return value_null();
	}
	int fill = 1;
	int thickness = 1;
	if (argc >= 4) fill = (int)expect_int(interp, args[3], "POLYGON", line, col);
	if (argc >= 5) thickness = (int)expect_int(interp, args[4], "POLYGON", line, col);
	if (interp->error) {
		free(pts);
		value_free(out);
		return value_null();
	}
	if (thickness < 1) thickness = 1;
	draw_polygon(out.as.tns, pts, npts, color, fill != 0, thickness);
	free(pts);
	return out;
}

static Value op_ellipse(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 5, 7, "ELLIPSE", line, col)) return value_null();
	Value out = copy_image_checked(interp, args[0], "ELLIPSE", line, col);
	if (interp->error) return value_null();

	if (args[1].type != VAL_TNS || !args[1].as.tns || args[1].as.tns->ndim != 1 || args[1].as.tns->shape[0] != 2) {
		value_free(out);
		return fail(interp, "ELLIPSE center must be TNS[2]", line, col);
	}
	Value cxv = args[1].as.tns->data[0];
	Value cyv = args[1].as.tns->data[1];
	if (cxv.type != VAL_INT || cyv.type != VAL_INT) {
		value_free(out);
		return fail(interp, "ELLIPSE center coordinates must be INT", line, col);
	}
	/* Convert center from user (1-based) to internal (0-based) */
	int cx = (int)cxv.as.i - 1;
	int cy = (int)cyv.as.i - 1;
	int rx = (int)expect_int(interp, args[2], "ELLIPSE", line, col);
	int ry = (int)expect_int(interp, args[3], "ELLIPSE", line, col);
	if (interp->error) {
		value_free(out);
		return value_null();
	}
	int color[4];
	if (!parse_color_rgba(interp, args[4], color, "ELLIPSE", line, col)) {
		value_free(out);
		return value_null();
	}
	int fill = 1;
	int thickness = 1;
	if (argc >= 6) fill = (int)expect_int(interp, args[5], "ELLIPSE", line, col);
	if (argc >= 7) thickness = (int)expect_int(interp, args[6], "ELLIPSE", line, col);
	if (interp->error) {
		value_free(out);
		return value_null();
	}
	if (thickness < 1) thickness = 1;
	draw_ellipse(out.as.tns, cx, cy, rx, ry, color, fill != 0, thickness);
	return out;
}

static Value threshold_channel(Interpreter* interp, Value imgv, Value thv, Value colorv, int ch, const char* opname, int line, int col) {
	Value out = copy_image_checked(interp, imgv, opname, line, col);
	if (interp->error) return value_null();
	int th = (int)expect_int(interp, thv, opname, line, col);
	if (interp->error) {
		value_free(out);
		return value_null();
	}
	int color[4] = {0, 0, 0, 0};
	if (colorv.type != VAL_NULL) {
		if (!parse_color_rgba(interp, colorv, color, opname, line, col)) {
			value_free(out);
			return value_null();
		}
	}
	Tensor* t = out.as.tns;
	for (size_t x = 0; x < t->shape[0]; x++) {
		for (size_t y = 0; y < t->shape[1]; y++) {
			size_t off = pixel_offset(t, x, y);
			int v = (int)t->data[off + (size_t)ch].as.i;
			if (v <= th) {
				for (size_t c = 0; c < 4; c++) t->data[off + c] = value_int((int64_t)color[c]);
			}
		}
	}
	return out;
}

static Value op_threshhold_a(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "THRESHHOLD_A", line, col)) return value_null();
	return threshold_channel(interp, args[0], args[1], (argc >= 3) ? args[2] : value_null(), 3, "THRESHHOLD_A", line, col);
}

static Value op_threshhold_r(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "THRESHHOLD_R", line, col)) return value_null();
	return threshold_channel(interp, args[0], args[1], (argc >= 3) ? args[2] : value_null(), 0, "THRESHHOLD_R", line, col);
}

static Value op_threshhold_g(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "THRESHHOLD_G", line, col)) return value_null();
	return threshold_channel(interp, args[0], args[1], (argc >= 3) ? args[2] : value_null(), 1, "THRESHHOLD_G", line, col);
}

static Value op_threshhold_b(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 3, "THRESHHOLD_B", line, col)) return value_null();
	return threshold_channel(interp, args[0], args[1], (argc >= 3) ? args[2] : value_null(), 2, "THRESHHOLD_B", line, col);
}

static Value resize_impl(Interpreter* interp, Value imgv, int new_w, int new_h, int antialiasing, const char* opname, int line, int col) {
	ImageView iv;
	if (!image_from_value(interp, imgv, opname, line, col, &iv)) return value_null();
	if (new_w <= 0 || new_h <= 0) return fail(interp, "new dimensions must be > 0", line, col);

	Value out = make_image((size_t)new_w, (size_t)new_h);
	Tensor* st = iv.t;
	Tensor* dt = out.as.tns;

	double sx = (double)iv.w / (double)new_w;
	double sy = (double)iv.h / (double)new_h;

	for (int x = 0; x < new_w; x++) {
		for (int y = 0; y < new_h; y++) {
			double srcx = ((double)x + 0.5) * sx - 0.5;
			double srcy = ((double)y + 0.5) * sy - 0.5;
			size_t doff = pixel_offset(dt, (size_t)x, (size_t)y);

			if (!antialiasing) {
				int nx = (int)floor(srcx + 0.5);
				int ny = (int)floor(srcy + 0.5);
				if (nx < 0) nx = 0;
				if (ny < 0) ny = 0;
				if ((size_t)nx >= iv.w) nx = (int)iv.w - 1;
				if ((size_t)ny >= iv.h) ny = (int)iv.h - 1;
				size_t soff = pixel_offset(st, (size_t)nx, (size_t)ny);
				for (size_t c = 0; c < 4; c++) dt->data[doff + c] = value_int(st->data[soff + c].as.i);
			} else {
				int x0 = (int)floor(srcx);
				int y0 = (int)floor(srcy);
				int x1 = x0 + 1;
				int y1 = y0 + 1;
				double wx = srcx - (double)x0;
				double wy = srcy - (double)y0;
				if (x0 < 0) x0 = 0;
				if (y0 < 0) y0 = 0;
				if ((size_t)x1 >= iv.w) x1 = (int)iv.w - 1;
				if ((size_t)y1 >= iv.h) y1 = (int)iv.h - 1;
				for (size_t c = 0; c < 4; c++) {
					double v00 = (double)st->data[pixel_offset(st, (size_t)x0, (size_t)y0) + c].as.i;
					double v10 = (double)st->data[pixel_offset(st, (size_t)x1, (size_t)y0) + c].as.i;
					double v01 = (double)st->data[pixel_offset(st, (size_t)x0, (size_t)y1) + c].as.i;
					double v11 = (double)st->data[pixel_offset(st, (size_t)x1, (size_t)y1) + c].as.i;
					double v0 = v00 * (1.0 - wx) + v10 * wx;
					double v1 = v01 * (1.0 - wx) + v11 * wx;
					int outv = (int)floor(v0 * (1.0 - wy) + v1 * wy + 0.5);
					dt->data[doff + c] = value_int((int64_t)clamp_u8_i32(outv));
				}
			}
		}
	}
	return out;
}

static Value op_scale(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 3, 4, "SCALE", line, col)) return value_null();
	ImageView iv;
	if (!image_from_value(interp, args[0], "SCALE", line, col, &iv)) return value_null();
	double sx = expect_num(interp, args[1], "SCALE", line, col);
	double sy = expect_num(interp, args[2], "SCALE", line, col);
	if (interp->error) return value_null();
	int aa = 1;
	if (argc >= 4) aa = (int)expect_int(interp, args[3], "SCALE", line, col);
	if (interp->error) return value_null();
	if (sx <= 0.0 || sy <= 0.0) return fail(interp, "SCALE factors must be > 0", line, col);
	int nw = (int)floor((double)iv.w * sx + 0.5);
	int nh = (int)floor((double)iv.h * sy + 0.5);
	if (nw < 1) nw = 1;
	if (nh < 1) nh = 1;
	return resize_impl(interp, args[0], nw, nh, aa != 0, "SCALE", line, col);
}

static Value op_resize(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 3, 4, "RESIZE", line, col)) return value_null();
	int nw = (int)expect_int(interp, args[1], "RESIZE", line, col);
	int nh = (int)expect_int(interp, args[2], "RESIZE", line, col);
	if (interp->error) return value_null();
	int aa = 1;
	if (argc >= 4) aa = (int)expect_int(interp, args[3], "RESIZE", line, col);
	if (interp->error) return value_null();
	return resize_impl(interp, args[0], nw, nh, aa != 0, "RESIZE", line, col);
}

static Value op_rotate(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 2, "ROTATE", line, col)) return value_null();
	ImageView iv;
	if (!image_from_value(interp, args[0], "ROTATE", line, col, &iv)) return value_null();
	double deg = expect_num(interp, args[1], "ROTATE", line, col);
	if (interp->error) return value_null();

	Value out = make_image(iv.w, iv.h);
	Tensor* st = iv.t;
	Tensor* dt = out.as.tns;
	double rad = -deg * (3.14159265358979323846 / 180.0);
	double cs = cos(rad);
	double sn = sin(rad);
	double cx = ((double)iv.w - 1.0) * 0.5;
	double cy = ((double)iv.h - 1.0) * 0.5;

	for (size_t x = 0; x < iv.w; x++) {
		for (size_t y = 0; y < iv.h; y++) {
			double dx = (double)x - cx;
			double dy = (double)y - cy;
			double sx = cx + dx * cs - dy * sn;
			double sy = cy + dx * sn + dy * cs;
			size_t doff = pixel_offset(dt, x, y);
			int ix = (int)floor(sx + 0.5);
			int iy = (int)floor(sy + 0.5);
			if (ix >= 0 && iy >= 0 && (size_t)ix < iv.w && (size_t)iy < iv.h) {
				size_t soff = pixel_offset(st, (size_t)ix, (size_t)iy);
				for (size_t c = 0; c < 4; c++) dt->data[doff + c] = value_int(st->data[soff + c].as.i);
			} else {
				dt->data[doff + 0] = value_int(0);
				dt->data[doff + 1] = value_int(0);
				dt->data[doff + 2] = value_int(0);
				dt->data[doff + 3] = value_int(0);
			}
		}
	}
	return out;
}

static Value op_blit(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 4, 5, "BLIT", line, col)) return value_null();
	ImageView src;
	ImageView dst;
	if (!image_from_value(interp, args[0], "BLIT", line, col, &src)) return value_null();
	if (!image_from_value(interp, args[1], "BLIT", line, col, &dst)) return value_null();
	int ox = (int)expect_int(interp, args[2], "BLIT", line, col);
	int oy = (int)expect_int(interp, args[3], "BLIT", line, col);
	/* Convert origin from user (1-based) to internal (0-based) */
	ox = ox - 1;
	oy = oy - 1;
	if (interp->error) return value_null();
	int mix = 1;
	if (argc >= 5) mix = (int)expect_int(interp, args[4], "BLIT", line, col);
	if (interp->error) return value_null();

	Value out = copy_image_checked(interp, args[1], "BLIT", line, col);
	if (interp->error) return value_null();
	Tensor* dt = out.as.tns;

	for (size_t sx = 0; sx < src.w; sx++) {
		for (size_t sy = 0; sy < src.h; sy++) {
			int dx = (int)sx + ox;
			int dy = (int)sy + oy;
			if (dx < 0 || dy < 0 || (size_t)dx >= dst.w || (size_t)dy >= dst.h) continue;
			size_t soff = pixel_offset(src.t, sx, sy);
			int rgba[4] = {
				(int)src.t->data[soff + 0].as.i,
				(int)src.t->data[soff + 1].as.i,
				(int)src.t->data[soff + 2].as.i,
				(int)src.t->data[soff + 3].as.i
			};
			put_pixel_rgba(dt, dx, dy, rgba, mix != 0);
		}
	}
	return out;
}

static Value op_grayscale(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 1, 1, "GRAYSCALE", line, col)) return value_null();
	Value out = copy_image_checked(interp, args[0], "GRAYSCALE", line, col);
	if (interp->error) return value_null();
	Tensor* t = out.as.tns;
	for (size_t x = 0; x < t->shape[0]; x++) {
		for (size_t y = 0; y < t->shape[1]; y++) {
			size_t off = pixel_offset(t, x, y);
			int r = (int)t->data[off + 0].as.i;
			int g = (int)t->data[off + 1].as.i;
			int b = (int)t->data[off + 2].as.i;
			int l = clamp_u8_i32((299 * r + 587 * g + 114 * b) / 1000);
			t->data[off + 0] = value_int(l);
			t->data[off + 1] = value_int(l);
			t->data[off + 2] = value_int(l);
		}
	}
	return out;
}

static Value op_replace_color(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 3, 3, "REPLACE_COLOR", line, col)) return value_null();
	int target[4];
	int repl[4];
	if (!parse_color_rgba(interp, args[1], target, "REPLACE_COLOR", line, col)) return value_null();
	if (!parse_color_rgba(interp, args[2], repl, "REPLACE_COLOR", line, col)) return value_null();
	Value out = copy_image_checked(interp, args[0], "REPLACE_COLOR", line, col);
	if (interp->error) return value_null();
	Tensor* t = out.as.tns;
	for (size_t x = 0; x < t->shape[0]; x++) {
		for (size_t y = 0; y < t->shape[1]; y++) {
			size_t off = pixel_offset(t, x, y);
			int same = 1;
			for (size_t c = 0; c < 4; c++) {
				if ((int)t->data[off + c].as.i != target[c]) {
					same = 0;
					break;
				}
			}
			if (same) {
				for (size_t c = 0; c < 4; c++) t->data[off + c] = value_int((int64_t)repl[c]);
			}
		}
	}
	return out;
}

static Value op_blur(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 2, "BLUR", line, col)) return value_null();
	int radius = (int)expect_int(interp, args[1], "BLUR", line, col);
	if (interp->error) return value_null();
	if (radius < 0) return fail(interp, "BLUR radius must be >= 0", line, col);
	if (radius == 0) return copy_image_checked(interp, args[0], "BLUR", line, col);

	ImageView iv;
	if (!image_from_value(interp, args[0], "BLUR", line, col, &iv)) return value_null();
	Value temp = make_image(iv.w, iv.h);
	Value out = make_image(iv.w, iv.h);
	Tensor* st = iv.t;
	Tensor* tt = temp.as.tns;
	Tensor* dt = out.as.tns;

	for (size_t x = 0; x < iv.w; x++) {
		for (size_t y = 0; y < iv.h; y++) {
			size_t off = pixel_offset(tt, x, y);
			for (size_t c = 0; c < 4; c++) {
				int sum = 0;
				int cnt = 0;
				for (int k = -radius; k <= radius; k++) {
					int sx = (int)x + k;
					if (sx < 0 || (size_t)sx >= iv.w) continue;
					sum += (int)st->data[pixel_offset(st, (size_t)sx, y) + c].as.i;
					cnt++;
				}
				tt->data[off + c] = value_int((cnt > 0) ? (sum / cnt) : 0);
			}
		}
	}

	for (size_t x = 0; x < iv.w; x++) {
		for (size_t y = 0; y < iv.h; y++) {
			size_t off = pixel_offset(dt, x, y);
			for (size_t c = 0; c < 4; c++) {
				int sum = 0;
				int cnt = 0;
				for (int k = -radius; k <= radius; k++) {
					int sy = (int)y + k;
					if (sy < 0 || (size_t)sy >= iv.h) continue;
					sum += (int)tt->data[pixel_offset(tt, x, (size_t)sy) + c].as.i;
					cnt++;
				}
				dt->data[off + c] = value_int((cnt > 0) ? (sum / cnt) : 0);
			}
		}
	}

	value_free(temp);
	return out;
}

static Value op_edge(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 1, 1, "EDGE", line, col)) return value_null();
	Value blur1_args[2] = {args[0], value_int(1)};
	Value blur2_args[2] = {args[0], value_int(2)};
	Value b1 = op_blur(interp, blur1_args, 2, NULL, NULL, line, col);
	if (interp->error) return value_null();
	Value b2 = op_blur(interp, blur2_args, 2, NULL, NULL, line, col);
	if (interp->error) {
		value_free(b1);
		return value_null();
	}

	ImageView i1;
	ImageView i2;
	if (!image_from_value(interp, b1, "EDGE", line, col, &i1) || !image_from_value(interp, b2, "EDGE", line, col, &i2)) {
		value_free(b1);
		value_free(b2);
		return value_null();
	}
	Value out = make_image(i1.w, i1.h);
	Tensor* dt = out.as.tns;

	for (size_t x = 0; x < i1.w; x++) {
		for (size_t y = 0; y < i1.h; y++) {
			size_t o1 = pixel_offset(i1.t, x, y);
			size_t o2 = pixel_offset(i2.t, x, y);
			size_t od = pixel_offset(dt, x, y);
			int g1 = ((int)i1.t->data[o1 + 0].as.i + (int)i1.t->data[o1 + 1].as.i + (int)i1.t->data[o1 + 2].as.i) / 3;
			int g2 = ((int)i2.t->data[o2 + 0].as.i + (int)i2.t->data[o2 + 1].as.i + (int)i2.t->data[o2 + 2].as.i) / 3;
			int e = abs(g1 - g2);
			if (e > 255) e = 255;
			dt->data[od + 0] = value_int(e);
			dt->data[od + 1] = value_int(e);
			dt->data[od + 2] = value_int(e);
			dt->data[od + 3] = value_int((int64_t)i1.t->data[o1 + 3].as.i);
		}
	}

	value_free(b1);
	value_free(b2);
	return out;
}

static int parse_palette(Interpreter* interp, Value v, int** out_colors, size_t* out_n, int line, int col) {
	if (!out_colors || !out_n) return 0;
	*out_colors = NULL;
	*out_n = 0;
	if (v.type != VAL_TNS || !v.as.tns) {
		set_runtime_error(interp, "CELLSHADE palette must be TNS", line, col);
		return 0;
	}
	Tensor* t = v.as.tns;
	if (t->ndim == 1 && (t->shape[0] == 3 || t->shape[0] == 4)) {
		int* cols = (int*)malloc(sizeof(int) * 4);
		if (!cols) {
			set_runtime_error(interp, "out of memory", line, col);
			return 0;
		}
		for (size_t c = 0; c < 4; c++) cols[c] = (c < t->shape[0] && t->data[c].type == VAL_INT) ? clamp_u8_i64(t->data[c].as.i) : 255;
		if (t->shape[0] == 3) cols[3] = 255;
		*out_colors = cols;
		*out_n = 1;
		return 1;
	}
	if (t->ndim == 2 && (t->shape[1] == 3 || t->shape[1] == 4) && t->shape[0] >= 1) {
		size_t n = t->shape[0];
		int* cols = (int*)malloc(sizeof(int) * n * 4);
		if (!cols) {
			set_runtime_error(interp, "out of memory", line, col);
			return 0;
		}
		for (size_t i = 0; i < n; i++) {
			size_t off = i * t->strides[0];
			for (size_t c = 0; c < t->shape[1]; c++) {
				Value e = t->data[off + c];
				if (e.type != VAL_INT) {
					free(cols);
					set_runtime_error(interp, "palette channels must be INT", line, col);
					return 0;
				}
				cols[i * 4 + c] = clamp_u8_i64(e.as.i);
			}
			if (t->shape[1] == 3) cols[i * 4 + 3] = 255;
		}
		*out_colors = cols;
		*out_n = n;
		return 1;
	}
	set_runtime_error(interp, "CELLSHADE palette must be [N,3|4] or [3|4]", line, col);
	return 0;
}

static Value op_cellshade(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (!expect_argc_range(interp, argc, 2, 2, "CELLSHADE", line, col)) return value_null();
	int* palette = NULL;
	size_t pal_n = 0;
	if (!parse_palette(interp, args[1], &palette, &pal_n, line, col)) return value_null();

	Value out = copy_image_checked(interp, args[0], "CELLSHADE", line, col);
	if (interp->error) {
		free(palette);
		return value_null();
	}
	Tensor* t = out.as.tns;
	for (size_t x = 0; x < t->shape[0]; x++) {
		for (size_t y = 0; y < t->shape[1]; y++) {
			size_t off = pixel_offset(t, x, y);
			int r = (int)t->data[off + 0].as.i;
			int g = (int)t->data[off + 1].as.i;
			int b = (int)t->data[off + 2].as.i;
			size_t best = 0;
			int64_t bestd = INT64_MAX;
			for (size_t i = 0; i < pal_n; i++) {
				int pr = palette[i * 4 + 0];
				int pg = palette[i * 4 + 1];
				int pb = palette[i * 4 + 2];
				int dr = r - pr;
				int dg = g - pg;
				int db = b - pb;
				int64_t d = (int64_t)dr * dr + (int64_t)dg * dg + (int64_t)db * db;
				if (d < bestd) {
					bestd = d;
					best = i;
				}
			}
			t->data[off + 0] = value_int((int64_t)palette[best * 4 + 0]);
			t->data[off + 1] = value_int((int64_t)palette[best * 4 + 1]);
			t->data[off + 2] = value_int((int64_t)palette[best * 4 + 2]);
			t->data[off + 3] = value_int((int64_t)palette[best * 4 + 3]);
		}
	}
	free(palette);
	return out;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context* ctx) {
	if (!ctx) return;
	ctx->register_operator("LOAD_PNG", (prefix_operator_fn)op_load_png, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("LOAD_JPEG", (prefix_operator_fn)op_load_jpeg, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("LOAD_BMP", (prefix_operator_fn)op_load_bmp, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SAVE_PNG", (prefix_operator_fn)op_save_png, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SAVE_JPEG", (prefix_operator_fn)op_save_jpeg, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SAVE_BMP", (prefix_operator_fn)op_save_bmp, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("POLYGON", (prefix_operator_fn)op_polygon, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("ELLIPSE", (prefix_operator_fn)op_ellipse, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("THRESHHOLD_A", (prefix_operator_fn)op_threshhold_a, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("THRESHHOLD_R", (prefix_operator_fn)op_threshhold_r, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("THRESHHOLD_G", (prefix_operator_fn)op_threshhold_g, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("THRESHHOLD_B", (prefix_operator_fn)op_threshhold_b, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SCALE", (prefix_operator_fn)op_scale, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("RESIZE", (prefix_operator_fn)op_resize, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("ROTATE", (prefix_operator_fn)op_rotate, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("BLIT", (prefix_operator_fn)op_blit, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("GRAYSCALE", (prefix_operator_fn)op_grayscale, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("REPLACE_COLOR", (prefix_operator_fn)op_replace_color, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("BLUR", (prefix_operator_fn)op_blur, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("EDGE", (prefix_operator_fn)op_edge, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("CELLSHADE", (prefix_operator_fn)op_cellshade, PREFIX_EXTENSION_ASMODULE);
}