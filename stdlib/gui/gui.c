#include "../../src/prefix_extension.h"
#include "../../src/interpreter.h"
#include "../../src/value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifdef _WIN32

typedef struct {
	int id;
	HWND hwnd;
	int scale_to_fit;
	char kind[32];
	uint8_t* image_rgba;
	int image_w;
	int image_h;
} GuiWindow;

typedef struct {
	GuiWindow* items;
	size_t count;
	size_t cap;
	int next_id;
	int class_registered;
	HINSTANCE hinstance;
} GuiState;

static GuiState g_gui = {0};
static const wchar_t* k_gui_class_name = L"PrefixGuiWindowClass";

static void set_runtime_error(Interpreter* interp, const char* msg, int line, int col) {
	if (!interp) return;
	if (interp->error) free(interp->error);
	interp->error = msg ? strdup(msg) : NULL;
	interp->error_line = line;
	interp->error_col = col;
}

static Value fail(Interpreter* interp, const char* msg, int line, int col) {
	set_runtime_error(interp, msg, line, col);
	return value_null();
}

static wchar_t* utf8_to_wide_dup(const char* s) {
	const char* in = s ? s : "";
	int wlen = MultiByteToWideChar(CP_UTF8, 0, in, -1, NULL, 0);
	if (wlen <= 0) return NULL;
	wchar_t* out = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
	if (!out) return NULL;
	if (MultiByteToWideChar(CP_UTF8, 0, in, -1, out, wlen) <= 0) {
		free(out);
		return NULL;
	}
	return out;
}

static void gui_window_free_image(GuiWindow* win) {
	if (!win) return;
	free(win->image_rgba);
	win->image_rgba = NULL;
	win->image_w = 0;
	win->image_h = 0;
}

static GuiWindow* gui_find_window_by_id(int id) {
	for (size_t i = 0; i < g_gui.count; i++) {
		if (g_gui.items[i].id == id) return &g_gui.items[i];
	}
	return NULL;
}

static GuiWindow* gui_find_window_by_hwnd(HWND hwnd) {
	for (size_t i = 0; i < g_gui.count; i++) {
		if (g_gui.items[i].hwnd == hwnd) return &g_gui.items[i];
	}
	return NULL;
}

static void gui_remove_window_by_index(size_t idx) {
	if (idx >= g_gui.count) return;
	gui_window_free_image(&g_gui.items[idx]);
	g_gui.items[idx] = g_gui.items[g_gui.count - 1];
	g_gui.count--;
}

static void gui_remove_window_by_hwnd(HWND hwnd) {
	for (size_t i = 0; i < g_gui.count; i++) {
		if (g_gui.items[i].hwnd == hwnd) {
			gui_remove_window_by_index(i);
			return;
		}
	}
}

static int gui_reserve(size_t need) {
	if (g_gui.cap >= need) return 1;
	size_t next = g_gui.cap == 0 ? 8 : g_gui.cap * 2;
	while (next < need) next *= 2;
	GuiWindow* p = (GuiWindow*)realloc(g_gui.items, sizeof(GuiWindow) * next);
	if (!p) return 0;
	g_gui.items = p;
	g_gui.cap = next;
	return 1;
}

static void gui_pump_messages(void) {
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

static int clamp_u8_i64(int64_t v) {
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (int)v;
}

static int64_t expect_int(Interpreter* interp, Value v, const char* opname, int line, int col, int* ok) {
	if (v.type != VAL_INT) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects INT argument", opname);
		set_runtime_error(interp, buf, line, col);
		if (ok) *ok = 0;
		return 0;
	}
	if (ok) *ok = 1;
	return v.as.i;
}

static const char* expect_str(Interpreter* interp, Value v, const char* opname, int line, int col, int* ok) {
	if (v.type != VAL_STR) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s expects STR argument", opname);
		set_runtime_error(interp, buf, line, col);
		if (ok) *ok = 0;
		return NULL;
	}
	if (ok) *ok = 1;
	return v.as.s ? v.as.s : "";
}

static int window_client_size(HWND hwnd, int* out_w, int* out_h) {
	RECT rc;
	if (!GetClientRect(hwnd, &rc)) return 0;
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
	return 1;
}

static void gui_draw_window(HDC hdc, GuiWindow* win, int dst_w, int dst_h) {
	if (!win || !hdc) return;
	if (!win->image_rgba || win->image_w <= 0 || win->image_h <= 0) {
		RECT rc = {0, 0, dst_w, dst_h};
		FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
		return;
	}

	int src_w = win->image_w;
	int src_h = win->image_h;
	size_t px_count = (size_t)src_w * (size_t)src_h;
	uint8_t* bgra = (uint8_t*)malloc(px_count * 4);
	if (!bgra) return;

	for (int y = 0; y < src_h; y++) {
		for (int x = 0; x < src_w; x++) {
			size_t i = ((size_t)y * (size_t)src_w + (size_t)x) * 4u;
			uint8_t r = win->image_rgba[i + 0];
			uint8_t g = win->image_rgba[i + 1];
			uint8_t b = win->image_rgba[i + 2];
			uint8_t a = win->image_rgba[i + 3];
			bgra[i + 0] = b;
			bgra[i + 1] = g;
			bgra[i + 2] = r;
			bgra[i + 3] = a;
		}
	}

	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = src_w;
	bmi.bmiHeader.biHeight = -src_h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	int out_w = src_w;
	int out_h = src_h;
	if (win->scale_to_fit) {
		out_w = dst_w;
		out_h = dst_h;
	}

	SetStretchBltMode(hdc, COLORONCOLOR);
	StretchDIBits(
		hdc,
		0,
		0,
		out_w,
		out_h,
		0,
		0,
		src_w,
		src_h,
		bgra,
		&bmi,
		DIB_RGB_COLORS,
		SRCCOPY
	);

	free(bgra);
}

static LRESULT CALLBACK gui_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			GuiWindow* win = gui_find_window_by_hwnd(hwnd);
			int cw = 1;
			int ch = 1;
			window_client_size(hwnd, &cw, &ch);
			gui_draw_window(hdc, win, cw, ch);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_SIZE: {
			InvalidateRect(hwnd, NULL, TRUE);
			return 0;
		}
		case WM_CLOSE: {
			DestroyWindow(hwnd);
			return 0;
		}
		case WM_NCDESTROY: {
			gui_remove_window_by_hwnd(hwnd);
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}
		default:
			return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

static int gui_ensure_class(void) {
	if (g_gui.class_registered) return 1;
	g_gui.hinstance = GetModuleHandleW(NULL);
	WNDCLASSW wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = gui_wnd_proc;
	wc.hInstance = g_gui.hinstance;
	wc.lpszClassName = k_gui_class_name;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	if (!RegisterClassW(&wc)) {
		return 0;
	}
	g_gui.class_registered = 1;
	if (g_gui.next_id <= 0) g_gui.next_id = 1;
	return 1;
}

static Value make_dims_tns(int w, int h) {
	size_t shape[1] = {2};
	Value out = value_tns_new(TYPE_INT, 1, shape);
	out.as.tns->data[0] = value_int((int64_t)w);
	out.as.tns->data[1] = value_int((int64_t)h);
	return out;
}

static int extract_image_rgba(Interpreter* interp, Value v, uint8_t** out_rgba, int* out_w, int* out_h, int line, int col) {
	if (!out_rgba || !out_w || !out_h) return 0;
	*out_rgba = NULL;
	*out_w = 0;
	*out_h = 0;

	if (v.type != VAL_TNS || !v.as.tns) {
		set_runtime_error(interp, "GUI_SHOW_IMAGE expects TNS image", line, col);
		return 0;
	}

	Tensor* t = v.as.tns;
	if (t->ndim != 3 || !(t->shape[2] == 3 || t->shape[2] == 4)) {
		set_runtime_error(interp, "GUI_SHOW_IMAGE expects an image tensor shaped [w][h][3|4]", line, col);
		return 0;
	}

	int w = (int)t->shape[0];
	int h = (int)t->shape[1];
	int c = (int)t->shape[2];
	if (w <= 0 || h <= 0) {
		set_runtime_error(interp, "GUI_SHOW_IMAGE expects non-empty image dimensions", line, col);
		return 0;
	}

	size_t total = (size_t)w * (size_t)h * 4u;
	uint8_t* rgba = (uint8_t*)malloc(total);
	if (!rgba) {
		set_runtime_error(interp, "GUI_SHOW_IMAGE failed: out of memory", line, col);
		return 0;
	}

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			size_t pixel_i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
			size_t base = (size_t)x * t->strides[0] + (size_t)y * t->strides[1];
			for (int ch = 0; ch < c; ch++) {
				Value e = t->data[base + (size_t)ch * t->strides[2]];
				if (e.type != VAL_INT) {
					free(rgba);
					set_runtime_error(interp, "GUI_SHOW_IMAGE failed: image tensor channels must be INT", line, col);
					return 0;
				}
				rgba[pixel_i + (size_t)ch] = (uint8_t)clamp_u8_i64(e.as.i);
			}
			if (c == 3) rgba[pixel_i + 3] = 255;
		}
	}

	*out_rgba = rgba;
	*out_w = w;
	*out_h = h;
	return 1;
}

static int parse_window_kind(Interpreter* interp, const char* kind, int* out_scale_default, DWORD* inout_style, DWORD* inout_exstyle, int* fullscreen, int line, int col) {
	if (!kind || !out_scale_default || !inout_style || !inout_exstyle || !fullscreen) return 0;

	char lowered[64];
	size_t n = strlen(kind);
	if (n >= sizeof(lowered)) n = sizeof(lowered) - 1;
	for (size_t i = 0; i < n; i++) {
		char c = kind[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		lowered[i] = c;
	}
	lowered[n] = '\0';

	*out_scale_default = 1;
	*fullscreen = 0;

	if (strcmp(lowered, "scaled") == 0 || strcmp(lowered, "resizable") == 0 || lowered[0] == '\0') {
		*inout_style |= (WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
		return 1;
	}
	if (strcmp(lowered, "fixed") == 0) {
		*inout_style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
		*inout_style |= WS_MINIMIZEBOX;
		*out_scale_default = 0;
		return 1;
	}
	if (strcmp(lowered, "fullscreen") == 0) {
		*inout_style = WS_POPUP | WS_VISIBLE;
		*inout_exstyle = WS_EX_APPWINDOW;
		*fullscreen = 1;
		return 1;
	}
	if (strcmp(lowered, "borderless") == 0) {
		*inout_style = WS_POPUP | WS_VISIBLE;
		*inout_exstyle = WS_EX_APPWINDOW;
		return 1;
	}

	{
		char msg[256];
		snprintf(msg, sizeof(msg), "GUI_CREATE_WINDOW: unknown window type '%s'", kind);
		set_runtime_error(interp, msg, line, col);
	}
	return 0;
}

static Value op_create_window(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();

	if (argc < 0 || argc > 5) {
		return fail(interp, "GUI_CREATE_WINDOW expects 0..5 arguments", line, col);
	}
	if (!gui_ensure_class()) {
		return fail(interp, "GUI_CREATE_WINDOW failed: unable to initialize window class", line, col);
	}

	const char* kind = "scaled";
	int width = 640;
	int height = 480;
	const char* title = "Prefix GUI";
	int scale_provided = (argc >= 5);
	int scale_flag = 1;

	int ok = 1;
	if (argc >= 1) {
		kind = expect_str(interp, args[0], "GUI_CREATE_WINDOW", line, col, &ok);
		if (!ok) return value_null();
	}
	if (argc >= 2) {
		width = (int)expect_int(interp, args[1], "GUI_CREATE_WINDOW", line, col, &ok);
		if (!ok) return value_null();
	}
	if (argc >= 3) {
		height = (int)expect_int(interp, args[2], "GUI_CREATE_WINDOW", line, col, &ok);
		if (!ok) return value_null();
	}
	if (argc >= 4) {
		title = expect_str(interp, args[3], "GUI_CREATE_WINDOW", line, col, &ok);
		if (!ok) return value_null();
	}
	if (argc >= 5) {
		scale_flag = (int)expect_int(interp, args[4], "GUI_CREATE_WINDOW", line, col, &ok);
		if (!ok) return value_null();
	}

	if (width < 1) width = 1;
	if (height < 1) height = 1;

	DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	DWORD exstyle = WS_EX_APPWINDOW;
	int kind_scale_default = 1;
	int fullscreen = 0;
	if (!parse_window_kind(interp, kind, &kind_scale_default, &style, &exstyle, &fullscreen, line, col)) {
		return value_null();
	}
	int scale_to_fit = scale_provided ? (scale_flag != 0) : kind_scale_default;

	wchar_t* wtitle = utf8_to_wide_dup(title);
	if (!wtitle) return fail(interp, "GUI_CREATE_WINDOW failed: invalid title encoding", line, col);

	RECT wr = {0, 0, width, height};
	AdjustWindowRectEx(&wr, style, FALSE, exstyle);
	int win_w = wr.right - wr.left;
	int win_h = wr.bottom - wr.top;

	HWND hwnd = CreateWindowExW(
		exstyle,
		k_gui_class_name,
		wtitle,
		style,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		win_w,
		win_h,
		NULL,
		NULL,
		g_gui.hinstance,
		NULL
	);
	free(wtitle);

	if (!hwnd) return fail(interp, "GUI_CREATE_WINDOW failed: CreateWindowExW failed", line, col);

	if (fullscreen) {
		int sw = GetSystemMetrics(SM_CXSCREEN);
		int sh = GetSystemMetrics(SM_CYSCREEN);
		SetWindowPos(hwnd, HWND_TOP, 0, 0, sw, sh, SWP_SHOWWINDOW);
	}

	if (!gui_reserve(g_gui.count + 1)) {
		DestroyWindow(hwnd);
		return fail(interp, "GUI_CREATE_WINDOW failed: out of memory", line, col);
	}

	GuiWindow gw;
	memset(&gw, 0, sizeof(gw));
	gw.id = g_gui.next_id++;
	gw.hwnd = hwnd;
	gw.scale_to_fit = scale_to_fit;
	{
		size_t kn = strlen(kind);
		if (kn >= sizeof(gw.kind)) kn = sizeof(gw.kind) - 1;
		memcpy(gw.kind, kind, kn);
		gw.kind[kn] = '\0';
	}

	g_gui.items[g_gui.count++] = gw;

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
	gui_pump_messages();

	return value_int((int64_t)gw.id);
}

static Value op_show_image(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();

	if (argc < 2) return fail(interp, "GUI_SHOW_IMAGE expects 2 arguments", line, col);

	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_SHOW_IMAGE", line, col, &ok);
	if (!ok) return value_null();

	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) {
		return fail(interp, "GUI_SHOW_IMAGE: invalid window handle", line, col);
	}

	uint8_t* rgba = NULL;
	int w = 0, h = 0;
	if (!extract_image_rgba(interp, args[1], &rgba, &w, &h, line, col)) {
		return value_null();
	}

	gui_window_free_image(win);
	win->image_rgba = rgba;
	win->image_w = w;
	win->image_h = h;

	InvalidateRect(win->hwnd, NULL, TRUE);
	UpdateWindow(win->hwnd);
	gui_pump_messages();
	return value_int(1);
}

static Value op_close_window(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();

	if (argc < 1) return fail(interp, "GUI_CLOSE_WINDOW expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_CLOSE_WINDOW", line, col, &ok);
	if (!ok) return value_null();

	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) {
		return fail(interp, "GUI_CLOSE_WINDOW: invalid window handle", line, col);
	}
	DestroyWindow(win->hwnd);
	gui_pump_messages();
	return value_int(1);
}

static Value op_minimize(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();
	if (argc < 1) return fail(interp, "GUI_MINIMIZE expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_MINIMIZE", line, col, &ok);
	if (!ok) return value_null();
	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) return fail(interp, "GUI_MINIMIZE: invalid window handle", line, col);
	ShowWindow(win->hwnd, SW_MINIMIZE);
	gui_pump_messages();
	return value_int(1);
}

static Value op_maximize(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();
	if (argc < 1) return fail(interp, "GUI_MAXIMIZE expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_MAXIMIZE", line, col, &ok);
	if (!ok) return value_null();
	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) return fail(interp, "GUI_MAXIMIZE: invalid window handle", line, col);
	ShowWindow(win->hwnd, SW_MAXIMIZE);
	gui_pump_messages();
	return value_int(1);
}

static Value op_to_front(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();
	if (argc < 1) return fail(interp, "GUI_TO_FRONT expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_TO_FRONT", line, col, &ok);
	if (!ok) return value_null();
	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) return fail(interp, "GUI_TO_FRONT: invalid window handle", line, col);
	SetWindowPos(win->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(win->hwnd);
	gui_pump_messages();
	return value_int(1);
}

static Value op_to_back(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();
	if (argc < 1) return fail(interp, "GUI_TO_BACK expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "GUI_TO_BACK", line, col, &ok);
	if (!ok) return value_null();
	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) return fail(interp, "GUI_TO_BACK: invalid window handle", line, col);
	SetWindowPos(win->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	gui_pump_messages();
	return value_int(1);
}

static Value op_screen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)interp;
	(void)args;
	(void)argc;
	(void)arg_nodes;
	(void)env;
	(void)line;
	(void)col;
	int w = GetSystemMetrics(SM_CXSCREEN);
	int h = GetSystemMetrics(SM_CYSCREEN);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	return make_dims_tns(w, h);
}

static Value op_window(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	gui_pump_messages();
	if (argc < 1) return fail(interp, "WINDOW expects 1 argument", line, col);
	int ok = 1;
	int wid = (int)expect_int(interp, args[0], "WINDOW", line, col, &ok);
	if (!ok) return value_null();
	GuiWindow* win = gui_find_window_by_id(wid);
	if (!win || !IsWindow(win->hwnd)) return fail(interp, "WINDOW: invalid window handle", line, col);

	RECT rc;
	if (!GetWindowRect(win->hwnd, &rc)) return fail(interp, "WINDOW: failed to query window size", line, col);
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	return make_dims_tns(w, h);
}

static Value op_screenshot(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)args;
	(void)argc;
	(void)arg_nodes;
	(void)env;

	HDC hdc_screen = GetDC(NULL);
	if (!hdc_screen) return fail(interp, "SCREENSHOT failed: GetDC failed", line, col);
	HDC hdc_mem = CreateCompatibleDC(hdc_screen);
	if (!hdc_mem) {
		ReleaseDC(NULL, hdc_screen);
		return fail(interp, "SCREENSHOT failed: CreateCompatibleDC failed", line, col);
	}

	int width = GetSystemMetrics(SM_CXSCREEN);
	int height = GetSystemMetrics(SM_CYSCREEN);
	if (width <= 0 || height <= 0) {
		DeleteDC(hdc_mem);
		ReleaseDC(NULL, hdc_screen);
		return fail(interp, "SCREENSHOT failed: invalid screen size", line, col);
	}

	HBITMAP hbmp = CreateCompatibleBitmap(hdc_screen, width, height);
	if (!hbmp) {
		DeleteDC(hdc_mem);
		ReleaseDC(NULL, hdc_screen);
		return fail(interp, "SCREENSHOT failed: CreateCompatibleBitmap failed", line, col);
	}
	HGDIOBJ old = SelectObject(hdc_mem, hbmp);

	if (!BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, 0, 0, SRCCOPY)) {
		SelectObject(hdc_mem, old);
		DeleteObject(hbmp);
		DeleteDC(hdc_mem);
		ReleaseDC(NULL, hdc_screen);
		return fail(interp, "SCREENSHOT failed: BitBlt failed", line, col);
	}

	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	size_t buf_size = (size_t)width * (size_t)height * 4u;
	uint8_t* buf = (uint8_t*)malloc(buf_size);
	if (!buf) {
		SelectObject(hdc_mem, old);
		DeleteObject(hbmp);
		DeleteDC(hdc_mem);
		ReleaseDC(NULL, hdc_screen);
		return fail(interp, "SCREENSHOT failed: out of memory", line, col);
	}

	int got = GetDIBits(hdc_mem, hbmp, 0, (UINT)height, buf, &bmi, DIB_RGB_COLORS);

	SelectObject(hdc_mem, old);
	DeleteObject(hbmp);
	DeleteDC(hdc_mem);
	ReleaseDC(NULL, hdc_screen);

	if (got == 0) {
		free(buf);
		return fail(interp, "SCREENSHOT failed: GetDIBits failed", line, col);
	}

	size_t shape[3];
	shape[0] = (size_t)width;
	shape[1] = (size_t)height;
	shape[2] = 4;
	Value out = value_tns_new(TYPE_INT, 3, shape);
	Tensor* t = out.as.tns;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t src_i = ((size_t)y * (size_t)width + (size_t)x) * 4u;
			uint8_t b = buf[src_i + 0];
			uint8_t g = buf[src_i + 1];
			uint8_t r = buf[src_i + 2];
			uint8_t a = buf[src_i + 3];

			size_t base = (size_t)x * t->strides[0] + (size_t)y * t->strides[1];
			t->data[base + 0 * t->strides[2]] = value_int((int64_t)r);
			t->data[base + 1 * t->strides[2]] = value_int((int64_t)g);
			t->data[base + 2 * t->strides[2]] = value_int((int64_t)b);
			t->data[base + 3 * t->strides[2]] = value_int((int64_t)a);
		}
	}

	free(buf);
	return out;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context* ctx) {
	if (!ctx) return;
	ctx->register_operator("CREATE_WINDOW", (prefix_operator_fn)op_create_window, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SHOW_IMAGE", (prefix_operator_fn)op_show_image, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("CLOSE_WINDOW", (prefix_operator_fn)op_close_window, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SCREEN", (prefix_operator_fn)op_screen, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WINDOW", (prefix_operator_fn)op_window, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("SCREENSHOT", (prefix_operator_fn)op_screenshot, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("MINIMIZE", (prefix_operator_fn)op_minimize, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("MAXIMIZE", (prefix_operator_fn)op_maximize, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TO_FRONT", (prefix_operator_fn)op_to_front, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("TO_BACK", (prefix_operator_fn)op_to_back, PREFIX_EXTENSION_ASMODULE);
}

#else

void prefix_extension_init(prefix_ext_context* ctx) {
	(void)ctx;
}

#endif
