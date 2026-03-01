#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/prefix_extension.h"
#include "../src/interpreter.h"
#include "../src/value.h"

// Minimal C implementation of the win32 extension. This implements a
// subset of the convenience operators provided by the Python version.

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

// Helper: get C string from Value (must be STR)
static const char* value_as_cstr(Interpreter* interp, Value v, const char* name, int line, int col) {
	(void)name;
	if (v.type != VAL_STR) {
		set_runtime_error(interp, "argument must be STR", line, col);
		return NULL;
	}
	return v.as.s;
}

// Helper: get integer from Value
static long long value_as_int(Interpreter* interp, Value v, const char* name, int line, int col) {
	(void)name;
	if (v.type != VAL_INT) {
		set_runtime_error(interp, "argument must be INT", line, col);
		return 0;
	}
	return v.as.i;
}

// Operators
static Value op_win_message_box(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 1 || argc > 2) {
		set_runtime_error(interp, "WIN_MESSAGE_BOX requires 1 or 2 args", line, col);
		return value_null();
	}
	const char* text = value_as_cstr(interp, args[0], "text", line, col);
	if (!text) return value_null();
	const char* title = "";
	if (argc == 2) {
		title = value_as_cstr(interp, args[1], "title", line, col);
		if (!title) return value_null();
	}
	// Convert to wide strings
	int tlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	wchar_t* wt = malloc(sizeof(wchar_t) * (tlen > 0 ? tlen : 1));
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wt, tlen > 0 ? tlen : 1);
	int slen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
	wchar_t* ws = malloc(sizeof(wchar_t) * (slen > 0 ? slen : 1));
	MultiByteToWideChar(CP_UTF8, 0, title, -1, ws, slen > 0 ? slen : 1);
	// Resolve MessageBoxW dynamically to avoid linking against user32.lib
	HMODULE user32 = LoadLibraryA("user32.dll");
	if (user32) {
		typedef int (WINAPI *MsgBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
		MsgBoxW_t pMsg = (MsgBoxW_t)GetProcAddress(user32, "MessageBoxW");
		if (pMsg) {
			int res = pMsg(NULL, wt, ws, MB_OK);
			FreeLibrary(user32);
			free(wt); free(ws);
			return value_int((int64_t)res);
		}
		FreeLibrary(user32);
	}
	// Fall back: if we couldn't resolve, free buffers and return 0
	free(wt); free(ws);
	return value_int(0);
}

static Value op_win_sleep(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 1) {
		set_runtime_error(interp, "WIN_SLEEP requires 1 arg (milliseconds)", line, col);
		return value_null();
	}
	long long ms = value_as_int(interp, args[0], "ms", line, col);
	if (interp->error) return value_null();
	Sleep((DWORD)ms);
	return value_int(0);
}

static Value op_win_last_error(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)interp; (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
	DWORD e = GetLastError();
	return value_int((int64_t)e);
}

static Value op_win_load_library(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 1) {
		set_runtime_error(interp, "WIN_LOAD_LIBRARY requires 1 arg", line, col);
		return value_null();
	}
	const char* name = value_as_cstr(interp, args[0], "name", line, col);
	if (!name) return value_null();
	// Accept both with and without .dll
	char buf[1024];
	if (_stricmp(name + (strlen(name) >= 4 ? strlen(name) - 4 : 0), ".dll") == 0) {
#ifdef _MSC_VER
		strcpy_s(buf, sizeof(buf), name);
#else
		strncpy(buf, name, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
#endif
	} else {
		snprintf(buf, sizeof(buf), "%s.dll", name);
	}
	HMODULE h = LoadLibraryA(buf);
	if (!h) {
		DWORD err = GetLastError();
		char em[128]; snprintf(em, sizeof(em), "LoadLibrary failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	return value_int((int64_t)(intptr_t)h);
}

static Value op_win_free_library(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 1) {
		set_runtime_error(interp, "WIN_FREE_LIBRARY requires 1 arg (handle id)", line, col);
		return value_null();
	}
	long long h = value_as_int(interp, args[0], "handle", line, col);
	if (interp->error) return value_null();
	BOOL ok = FreeLibrary((HMODULE)(intptr_t)h);
	if (!ok) {
		DWORD err = GetLastError();
		char em[128]; snprintf(em, sizeof(em), "FreeLibrary failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	return value_int((int64_t)ok);
}

static Value op_win_get_proc_address(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 2) {
		set_runtime_error(interp, "WIN_GET_PROC_ADDRESS requires 2 args (module_handle, proc_name)", line, col);
		return value_null();
	}
	long long mod = value_as_int(interp, args[0], "module", line, col);
	if (interp->error) return value_null();
	const char* name = value_as_cstr(interp, args[1], "proc_name", line, col);
	if (!name) return value_null();
	FARPROC p = GetProcAddress((HMODULE)(intptr_t)mod, name);
	if (!p) {
		DWORD err = GetLastError();
		char em[128]; snprintf(em, sizeof(em), "GetProcAddress failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	return value_int((int64_t)(intptr_t)p);
}

static Value op_win_create_file(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 5) {
		set_runtime_error(interp, "WIN_CREATE_FILE requires 5 args (path, access, share, creation, flags)", line, col);
		return value_null();
	}
	const char* path = value_as_cstr(interp, args[0], "path", line, col); if (!path) return value_null();
	long long access = value_as_int(interp, args[1], "access", line, col); if (interp->error) return value_null();
	long long share = value_as_int(interp, args[2], "share", line, col); if (interp->error) return value_null();
	long long creation = value_as_int(interp, args[3], "creation", line, col); if (interp->error) return value_null();
	long long flags = value_as_int(interp, args[4], "flags", line, col); if (interp->error) return value_null();
	// Convert path to wide
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	wchar_t* wp = malloc(sizeof(wchar_t) * (wlen > 0 ? wlen : 1));
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, wlen > 0 ? wlen : 1);
	HANDLE h = CreateFileW(wp, (DWORD)access, (DWORD)share, NULL, (DWORD)creation, (DWORD)flags, NULL);
	free(wp);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		char em[128]; snprintf(em, sizeof(em), "CreateFile failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	return value_int((int64_t)(intptr_t)h);
}

static Value op_win_read_file(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 2) {
		set_runtime_error(interp, "WIN_READ_FILE requires 2 args (handle, length)", line, col);
		return value_null();
	}
	long long handle = value_as_int(interp, args[0], "handle", line, col); if (interp->error) return value_null();
	long long length = value_as_int(interp, args[1], "length", line, col); if (interp->error) return value_null();
	if (length < 0) length = 0;
	char* buf = malloc((size_t)length + 1);
	if (!buf) { set_runtime_error(interp, "Out of memory", line, col); return value_null(); }
	DWORD read = 0;
	BOOL ok = ReadFile((HANDLE)(intptr_t)handle, buf, (DWORD)length, &read, NULL);
	if (!ok) {
		DWORD err = GetLastError();
		free(buf);
		char em[128]; snprintf(em, sizeof(em), "ReadFile failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	buf[read] = '\0';
	Value out = value_str(buf);
	free(buf);
	return out;
}

static Value op_win_write_file(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 2) {
		set_runtime_error(interp, "WIN_WRITE_FILE requires 2 args (handle, data)", line, col);
		return value_null();
	}
	long long handle = value_as_int(interp, args[0], "handle", line, col); if (interp->error) return value_null();
	// Accept STR for data
	if (args[1].type != VAL_STR) { set_runtime_error(interp, "data must be STR", line, col); return value_null(); }
	const char* data = args[1].as.s;
	DWORD towrite = (DWORD)strlen(data);
	DWORD written = 0;
	BOOL ok = WriteFile((HANDLE)(intptr_t)handle, data, towrite, &written, NULL);
	if (!ok) {
		DWORD err = GetLastError();
		char em[128]; snprintf(em, sizeof(em), "WriteFile failed: %lu", (unsigned long)err);
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	return value_int((int64_t)written);
}

static Value op_win_close_handle(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 1) { set_runtime_error(interp, "WIN_CLOSE_HANDLE requires 1 arg (handle)", line, col); return value_null(); }
	long long handle = value_as_int(interp, args[0], "handle", line, col); if (interp->error) return value_null();
	BOOL ok = CloseHandle((HANDLE)(intptr_t)handle);
	if (!ok) { DWORD err = GetLastError(); char em[128]; snprintf(em, sizeof(em), "CloseHandle failed: %lu", (unsigned long)err); set_runtime_error(interp, em, line, col); return value_null(); }
	return value_int((int64_t)ok);
}

static Value op_win_virtual_alloc(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 3) { set_runtime_error(interp, "WIN_VIRTUAL_ALLOC requires 3 args", line, col); return value_null(); }
	size_t size = (size_t)value_as_int(interp, args[0], "size", line, col); if (interp->error) return value_null();
	DWORD alloc_type = (DWORD)value_as_int(interp, args[1], "alloc_type", line, col); if (interp->error) return value_null();
	DWORD protect = (DWORD)value_as_int(interp, args[2], "protect", line, col); if (interp->error) return value_null();
	LPVOID p = VirtualAlloc(NULL, size, alloc_type, protect);
	if (!p) { DWORD err = GetLastError(); char em[128]; snprintf(em, sizeof(em), "VirtualAlloc failed: %lu", (unsigned long)err); set_runtime_error(interp, em, line, col); return value_null(); }
	return value_int((int64_t)(intptr_t)p);
}

static Value op_win_virtual_free(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 3) { set_runtime_error(interp, "WIN_VIRTUAL_FREE requires 3 args", line, col); return value_null(); }
	LPVOID addr = (LPVOID)(intptr_t)value_as_int(interp, args[0], "addr", line, col); if (interp->error) return value_null();
	SIZE_T size = (SIZE_T)value_as_int(interp, args[1], "size", line, col); if (interp->error) return value_null();
	DWORD free_type = (DWORD)value_as_int(interp, args[2], "free_type", line, col); if (interp->error) return value_null();
	BOOL ok = VirtualFree(addr, size, free_type);
	if (!ok) { DWORD err = GetLastError(); char em[128]; snprintf(em, sizeof(em), "VirtualFree failed: %lu", (unsigned long)err); set_runtime_error(interp, em, line, col); return value_null(); }
	return value_int((int64_t)ok);
}

static Value op_win_format_message(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	DWORD code = 0;
	if (argc >= 1) code = (DWORD)value_as_int(interp, args[0], "code", line, col);
	if (interp->error) return value_null();
	LPWSTR buf = NULL;
	DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
	DWORD len = FormatMessageW(flags, NULL, code, 0, (LPWSTR)&buf, 0, NULL);
	if (!len) {
		char em[128]; snprintf(em, sizeof(em), "FormatMessage failed: %lu", (unsigned long)GetLastError());
		set_runtime_error(interp, em, line, col);
		return value_null();
	}
	// Convert wide to UTF-8
	int clen = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
	char* out = malloc((size_t)(clen > 0 ? clen : 1));
	WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, clen > 0 ? clen : 1, NULL, NULL);
	LocalFree(buf);
	Value v = value_str(out);
	free(out);
	return v;
}

// A minimal WIN_CALL implementation: supports integer/pointer/string args and integer return.
// Only supports up to 6 arguments.
static Value op_win_call(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
	(void)arg_nodes;
	(void)env;
	if (argc < 4) { set_runtime_error(interp, "WIN_CALL requires at least 4 arguments", line, col); return value_null(); }
	// args: lib, func, arg_types, ret_type, ...
	const char* lib = value_as_cstr(interp, args[0], "lib", line, col); if (!lib) return value_null();
	const char* func = value_as_cstr(interp, args[1], "func", line, col); if (!func) return value_null();
	const char* arg_types = NULL; if (args[2].type == VAL_STR) arg_types = args[2].as.s; else { set_runtime_error(interp, "arg types must be STR", line, col); return value_null(); }
	const char* ret_type = NULL; if (args[3].type == VAL_STR) ret_type = args[3].as.s; else { set_runtime_error(interp, "ret type must be STR", line, col); return value_null(); }

	// Normalize library name
	char libbuf[1024]; if (strlen(lib) >= sizeof(libbuf)-5) { set_runtime_error(interp, "library name too long", line, col); return value_null(); }
	    if (_stricmp(lib + (strlen(lib) >= 4 ? strlen(lib) - 4 : 0), ".dll") == 0) {
	#ifdef _MSC_VER
		strcpy_s(libbuf, sizeof(libbuf), lib);
	#else
		strncpy(libbuf, lib, sizeof(libbuf)-1); libbuf[sizeof(libbuf)-1] = '\0';
	#endif
	    } else snprintf(libbuf, sizeof(libbuf), "%s.dll", lib);
	HMODULE h = LoadLibraryA(libbuf);
	if (!h) { char em[128]; snprintf(em, sizeof(em), "Failed to load DLL %s: %lu", libbuf, (unsigned long)GetLastError()); set_runtime_error(interp, em, line, col); return value_null(); }
	FARPROC p = GetProcAddress(h, func);
	if (!p) { char em[128]; snprintf(em, sizeof(em), "Function %s not found in %s: %lu", func, libbuf, (unsigned long)GetLastError()); set_runtime_error(interp, em, line, col); return value_null(); }

	// Prepare up to 6 args
	uintptr_t vargs[6] = {0};
	int provided = argc - 4;
	int code_count = 0;
	char codes[64];
#ifdef _MSC_VER
	strcpy_s(codes, sizeof(codes), arg_types);
#else
	strncpy(codes, arg_types, sizeof(codes)-1); codes[sizeof(codes)-1] = '\0';
#endif
	// if comma-separated, compact
	char compact[64]; int cc = 0;
	for (int i = 0; codes[i] && cc + 1 < (int)sizeof(compact); i++) if (codes[i] != ',') compact[cc++] = codes[i]; compact[cc] = '\0';
	code_count = (int)strlen(compact);
	if (code_count > 6) { set_runtime_error(interp, "WIN_CALL supports up to 6 arguments", line, col); return value_null(); }
	if (provided < code_count) { set_runtime_error(interp, "Not enough argument values for WIN_CALL", line, col); return value_null(); }
	for (int i = 0; i < code_count; i++) {
		char c = compact[i]; Value av = args[4 + i];
		if (c == 'I' || c == 'P') {
			if (av.type == VAL_INT) vargs[i] = (uintptr_t)av.as.i; else { set_runtime_error(interp, "expected INT arg", line, col); return value_null(); }
		} else if (c == 'S') {
			if (av.type != VAL_STR) { set_runtime_error(interp, "expected STR arg", line, col); return value_null(); }
			// convert to wchar and pass pointer
			int wlen = MultiByteToWideChar(CP_UTF8, 0, av.as.s, -1, NULL, 0);
			wchar_t* w = malloc(sizeof(wchar_t) * (wlen > 0 ? wlen : 1));
			MultiByteToWideChar(CP_UTF8, 0, av.as.s, -1, w, wlen > 0 ? wlen : 1);
			vargs[i] = (uintptr_t)w;
		} else if (c == 's') {
			if (av.type != VAL_STR) { set_runtime_error(interp, "expected STR arg", line, col); return value_null(); }
			vargs[i] = (uintptr_t)av.as.s;
		} else {
			set_runtime_error(interp, "unsupported arg type code in WIN_CALL", line, col); return value_null(); }
	}

	// Call function - support up to 6 integer/pointer args returning an integer
	typedef int (__stdcall *fn0_t)(void);
	typedef int (__stdcall *fn1_t)(uintptr_t);
	typedef int (__stdcall *fn2_t)(uintptr_t, uintptr_t);
	typedef int (__stdcall *fn3_t)(uintptr_t, uintptr_t, uintptr_t);
	typedef int (__stdcall *fn4_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	typedef int (__stdcall *fn5_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	typedef int (__stdcall *fn6_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	int cres = 0;
	switch (code_count) {
		case 0: cres = ((fn0_t)p)(); break;
		case 1: cres = ((fn1_t)p)(vargs[0]); break;
		case 2: cres = ((fn2_t)p)(vargs[0], vargs[1]); break;
		case 3: cres = ((fn3_t)p)(vargs[0], vargs[1], vargs[2]); break;
		case 4: cres = ((fn4_t)p)(vargs[0], vargs[1], vargs[2], vargs[3]); break;
		case 5: cres = ((fn5_t)p)(vargs[0], vargs[1], vargs[2], vargs[3], vargs[4]); break;
		case 6: cres = ((fn6_t)p)(vargs[0], vargs[1], vargs[2], vargs[3], vargs[4], vargs[5]); break;
		default: set_runtime_error(interp, "unsupported arg count", line, col); return value_null();
	}

	// Free any allocated wide strings used for string args
	for (int i = 0; i < code_count; i++) if (compact[i] == 'S' && vargs[i]) free((void*)vargs[i]);

	return value_int((int64_t)cres);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
// Register operators
void prefix_extension_init(prefix_ext_context* ctx) {
	if (!ctx) return;
	ctx->register_operator("WIN_CALL", (prefix_operator_fn)op_win_call, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_MESSAGE_BOX", (prefix_operator_fn)op_win_message_box, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_SLEEP", (prefix_operator_fn)op_win_sleep, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_LAST_ERROR", (prefix_operator_fn)op_win_last_error, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_LOAD_LIBRARY", (prefix_operator_fn)op_win_load_library, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_FREE_LIBRARY", (prefix_operator_fn)op_win_free_library, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_GET_PROC_ADDRESS", (prefix_operator_fn)op_win_get_proc_address, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_CREATE_FILE", (prefix_operator_fn)op_win_create_file, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_READ_FILE", (prefix_operator_fn)op_win_read_file, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_WRITE_FILE", (prefix_operator_fn)op_win_write_file, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_CLOSE_HANDLE", (prefix_operator_fn)op_win_close_handle, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_VIRTUAL_ALLOC", (prefix_operator_fn)op_win_virtual_alloc, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_VIRTUAL_FREE", (prefix_operator_fn)op_win_virtual_free, PREFIX_EXTENSION_ASMODULE);
	ctx->register_operator("WIN_FORMAT_MESSAGE", (prefix_operator_fn)op_win_format_message, PREFIX_EXTENSION_ASMODULE);
}
